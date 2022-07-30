/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#define MTF_MOCK_IMPL_cn_tree
#define MTF_MOCK_IMPL_cn_tree_compact
#define MTF_MOCK_IMPL_cn_tree_create
#define MTF_MOCK_IMPL_cn_tree_internal
#define MTF_MOCK_IMPL_cn_tree_iter
#define MTF_MOCK_IMPL_ct_view

#include <hse_util/alloc.h>
#include <hse_util/event_counter.h>
#include <hse_util/page.h>
#include <hse_util/slab.h>
#include <hse_util/mman.h>
#include <hse_util/list.h>
#include <hse_util/mutex.h>
#include <hse_util/logging.h>
#include <hse_util/assert.h>
#include <hse_util/parse_num.h>
#include <hse_util/atomic.h>
#include <hse_util/hlog.h>
#include <hse_util/table.h>
#include <hse_util/keycmp.h>
#include <hse_util/bin_heap.h>
#include <hse_util/log2.h>
#include <hse_util/workqueue.h>
#include <hse_util/compression_lz4.h>

#include <mpool/mpool.h>

#include <hse/limits.h>

#include <hse_ikvdb/key_hash.h>
#include <hse_ikvdb/limits.h>
#include <hse_ikvdb/cndb.h>
#include <hse_ikvdb/kvdb_health.h>
#include <hse_ikvdb/cn_tree_view.h>
#include <hse_ikvdb/cn.h>
#include <hse_ikvdb/cn_kvdb.h>
#include <hse_ikvdb/cursor.h>
#include <hse_ikvdb/sched_sts.h>
#include <hse_ikvdb/csched.h>
#include <hse_ikvdb/kvs_rparams.h>

#include <cn/cn_cursor.h>

#include "cn_tree.h"
#include "cn_tree_compact.h"
#include "cn_tree_create.h"
#include "cn_tree_iter.h"
#include "cn_tree_internal.h"

#include "cn_mblocks.h"
#include "cn_metrics.h"
#include "kvset.h"
#include "cn_perfc.h"
#include "kcompact.h"
#include "blk_list.h"
#include "kv_iterator.h"
#include "wbt_reader.h"
#include "spill.h"
#include "kcompact.h"
#include "kblock_builder.h"
#include "vblock_builder.h"
#include "route.h"
#include "kvset_internal.h"
#include "node_split.h"

static struct kmem_cache *cn_node_cache HSE_READ_MOSTLY;

static void
cn_setname(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

bool
cn_node_isleaf(const struct cn_tree_node *node);

/*----------------------------------------------------------------
 * SECTION: CN_TREE Traversal Utilities
 */


static size_t
cn_node_size(void)
{
    return ALIGN(sizeof(struct cn_tree_node), __alignof__(struct cn_tree_node));
}

struct cn_tree_node *
cn_node_alloc(struct cn_tree *tree, uint64_t nodeid)
{
    struct cn_tree_node *tn;

    tn = kmem_cache_zalloc(cn_node_cache);
    if (ev(!tn))
        return NULL;

    if (ev(hlog_create(&tn->tn_hlog, HLOG_PRECISION))) {
        kmem_cache_free(cn_node_cache, tn);
        return NULL;
    }

    INIT_LIST_HEAD(&tn->tn_link);
    INIT_LIST_HEAD(&tn->tn_kvset_list);
    INIT_LIST_HEAD(&tn->tn_rspills);
    mutex_init(&tn->tn_rspills_lock);

    atomic_init(&tn->tn_compacting, 0);
    atomic_init(&tn->tn_busycnt, 0);

    tn->tn_tree = tree;
    tn->tn_isroot = (nodeid == 0);
    tn->tn_nodeid = nodeid;

    tn->tn_size_max = tree->rp->cn_node_size_hi << 20;

    return tn;
}

void
cn_node_free(struct cn_tree_node *tn)
{
    if (tn) {
        hlog_destroy(tn->tn_hlog);
        kmem_cache_free(cn_node_cache, tn);
    }
}

/**
 * cn_tree_create() - add node to tree during initial tree creation
 *
 * This function is only to be used when building a cn_tree during start up.
 * To add a node during a spill operation, use cn_tree_add_kvset_to_node().
 */
merr_t
cn_tree_create(
    struct cn_tree **   handle,
    const char         *kvsname,
    u32                 cn_cflags,
    struct kvs_cparams *cp,
    struct kvdb_health *health,
    struct kvs_rparams *rp)
{
    struct cn_tree *tree;
    merr_t err;

    *handle = NULL;

    assert(health);

    if (ev(cp->fanout < CN_FANOUT_MIN || cp->fanout > CN_FANOUT_MAX))
        return merr(EINVAL);

    if (ev(cp->pfx_len > HSE_KVS_PFX_LEN_MAX))
        return merr(EINVAL);

    tree = alloc_aligned(sizeof(*tree), __alignof__(*tree));
    if (ev(!tree))
        return merr(ENOMEM);

    memset(tree, 0, sizeof(*tree));
    tree->ct_cp = cp;
    tree->ct_fanout = cp->fanout;
    tree->ct_pfx_len = cp->pfx_len;
    tree->ct_sfx_len = cp->sfx_len;
    tree->ct_kvdb_health = health;
    tree->rp = rp;

    INIT_LIST_HEAD(&tree->ct_nodes);

    tree->ct_root = cn_node_alloc(tree, 0);
    if (ev(!tree->ct_root)) {
        free_aligned(tree);
        return merr(ENOMEM);
    }

    list_add(&tree->ct_root->tn_link, &tree->ct_nodes);

    if (kvsname) {
        tree->ct_route_map = route_map_create(cp->fanout);
        if (!tree->ct_route_map) {
            cn_tree_destroy(tree);
            return merr(ENOMEM);
        }
    }

    tree->ct_i_nodec = 1;
    tree->ct_l_nodec = cp->fanout;
    tree->ct_lvl_max = 1;

    err = rmlock_init(&tree->ct_lock);
    if (err) {
        cn_tree_destroy(tree);
        return err;
    }

    /* setup cn_tree handle and return */
    *handle = tree;

    return 0;
}

static void
cn_node_destroy_cb(struct cn_work *work)
{
    struct kvset_list_entry *le, *tmp;
    struct cn_tree_node *node;

    node = container_of(work, struct cn_tree_node, tn_destroy_work);

    list_for_each_entry_safe(le, tmp, &node->tn_kvset_list, le_link)
        kvset_put_ref(le->le_kvset);

    cn_node_free(node);
}

void
cn_tree_destroy(struct cn_tree *tree)
{
    struct cn_tree_node *node, *next;

    if (!tree)
        return;

    /* Verify root node is at head of the list.
     */
    assert(tree->ct_root == list_first_entry(&tree->ct_nodes, typeof(*node), tn_link));

    /* Destroy root node last via safe reverse iteration of ct_nodes.
     */
    list_for_each_entry_reverse_safe(node, next, &tree->ct_nodes, tn_link) {
        if (node->tn_route_node)
            route_map_delete(tree->ct_route_map, node->tn_route_node);
        cn_work_submit(tree->cn, cn_node_destroy_cb, &node->tn_destroy_work);
    }

    /* Wait for async work to complete...
     */
    cn_ref_wait(tree->cn);

    rmlock_destroy(&tree->ct_lock);
    route_map_destroy(tree->ct_route_map);
    free_aligned(tree);
}

void
cn_tree_setup(
    struct cn_tree *    tree,
    struct mpool *      mp,
    struct cn *         cn,
    struct kvs_rparams *rp,
    struct cndb *       cndb,
    u64                 cnid,
    struct cn_kvdb *    cn_kvdb)
{
    tree->mp = mp;
    tree->cn = cn;
    tree->rp = rp;
    tree->cndb = cndb;
    tree->cnid = cnid;
    tree->cn_kvdb = cn_kvdb;
}

struct cn *
cn_tree_get_cn(const struct cn_tree *tree)
{
    return tree->cn;
}

struct cn_kvdb *
cn_tree_get_cnkvdb(const struct cn_tree *tree)
{
    return tree->cn_kvdb;
}

struct mpool *
cn_tree_get_mp(const struct cn_tree *tree)
{
    return tree->mp;
}

struct kvs_rparams *
cn_tree_get_rp(const struct cn_tree *tree)
{
    return tree->rp;
}

struct cndb *
cn_tree_get_cndb(const struct cn_tree *tree)
{
    return tree->cndb;
}

u64
cn_tree_get_cnid(const struct cn_tree *tree)
{
    return tree->cnid;
}

struct kvs_cparams *
cn_tree_get_cparams(const struct cn_tree *tree)
{
    return tree->ct_cp;
}

bool
cn_tree_is_replay(const struct cn_tree *tree)
{
    return cn_is_replay(tree->cn);
}

/*----------------------------------------------------------------
 * SECTION: CN_TREE Internal Functions to map node locations to node pointers
 */

/* Caller should hold tree read lock if consistent stats are desired */
void
cn_node_stats_get(const struct cn_tree_node *tn, struct cn_node_stats *s_out)
{
    *s_out = tn->tn_ns;
}

/* Helper for cn_tree_samp_* functions.  Do not use directly. */
static void
tn_samp_clear(struct cn_tree_node *tn)
{
    if (!cn_node_isleaf(tn) && tn->tn_hlog) {
        hlog_destroy(tn->tn_hlog);
        tn->tn_hlog = 0;
    } else if (tn->tn_hlog) {
        hlog_reset(tn->tn_hlog);
    }

    memset(&tn->tn_ns, 0, sizeof(tn->tn_ns));
    memset(&tn->tn_samp, 0, sizeof(tn->tn_samp));

    tn->tn_update_incr_dgen = 0;
}

/* Helper for cn_tree_samp_* functions.  Do not use directly. */
static bool
tn_samp_update_incr(struct cn_tree_node *tn, struct kvset *kvset, bool force)
{
    u64 dgen = kvset_get_dgen(kvset);

    if (!force && dgen <= tn->tn_update_incr_dgen)
        return false;

    if (tn->tn_hlog)
        hlog_union(tn->tn_hlog, kvset_get_hlog(kvset));

    kvset_stats_add(kvset_statsp(kvset), &tn->tn_ns.ns_kst);

    if (tn->tn_update_incr_dgen < dgen)
        tn->tn_update_incr_dgen = dgen;

    return true;
}

/* Helper for cn_tree_samp_* functions.  Do not use directly. */
static void
tn_samp_update_finish(struct cn_tree_node *tn)
{
    struct cn_node_stats *s = &tn->tn_ns;
    const uint            pct_scale = 1024;
    uint                  pct;
    const uint64_t num_keys = cn_ns_keys(s);

    /* Use hlog to estimate number of unique keys, but protect
     * against estimated values outside the valid range.
     * If no hlog, assume all keys are unique.
     */
    if (tn->tn_hlog) {
        s->ns_keys_uniq = hlog_card(tn->tn_hlog);
        if (s->ns_keys_uniq > num_keys)
            s->ns_keys_uniq = num_keys;
    } else {
        s->ns_keys_uniq = num_keys;
    }

    /* In the event that a node is composed of only prefix tombstones, it will
     * have 0 keys. Therefore protect against a division-by-zero error.
     */
    if (num_keys > 0) {
        pct = pct_scale * s->ns_keys_uniq / num_keys;
    } else {
        pct = pct_scale;
    }

    {
        u64 cur_alen = s->ns_kst.kst_kalen;
        u64 new_wlen = s->ns_kst.kst_kwlen * pct / pct_scale;
        enum hse_mclass mclass;
        u64 new_clen;

        mclass = cn_tree_node_mclass(tn, HSE_MPOLICY_DTYPE_KEY);
        assert(mclass != HSE_MCLASS_INVALID);
        new_clen = kbb_estimate_alen(tn->tn_tree->cn, new_wlen, mclass);
        s->ns_kclen = min(new_clen, cur_alen);
    }

    {
        u64 cur_alen = s->ns_kst.kst_valen;
        u64 cur_wlen = s->ns_kst.kst_vulen * pct / pct_scale;
        enum hse_mclass mclass;
        u64 new_clen;

        mclass = cn_tree_node_mclass(tn, HSE_MPOLICY_DTYPE_VALUE);
        assert(mclass != HSE_MCLASS_INVALID);
        new_clen = vbb_estimate_alen(tn->tn_tree->cn, cur_wlen, mclass);
        s->ns_vclen = min(new_clen, cur_alen);
    }

    s->ns_hclen = s->ns_kst.kst_halen;
    s->ns_pcap = min_t(u64, U16_MAX, 100 * cn_ns_clen(s) / tn->tn_size_max);

    tn->tn_samp.r_alen = 0;
    tn->tn_samp.r_wlen = 0;

    if (cn_node_isleaf(tn)) {
        tn->tn_samp.i_alen = 0;
        tn->tn_samp.l_alen = cn_ns_alen(s);
        tn->tn_samp.l_good = cn_ns_clen(s);
    } else {
        tn->tn_samp.i_alen = cn_ns_alen(s);
        tn->tn_samp.l_alen = 0;
        tn->tn_samp.l_good = 0;

        if (cn_node_isroot(tn)) {
            tn->tn_samp.r_alen = cn_ns_alen(s);
            tn->tn_samp.r_wlen = cn_ns_wlen(s);
        }
    }
}

/* This function must be serialized with other cn_tree_samp_* functions. */
static void
cn_tree_samp_update_compact(struct cn_tree *tree, struct cn_tree_node *tn)
{
    bool                     need_finish = false;
    struct cn_samp_stats     orig = tn->tn_samp;
    struct kvset_list_entry *le;

    tn_samp_clear(tn);

    list_for_each_entry (le, &tn->tn_kvset_list, le_link)
        if (tn_samp_update_incr(tn, le->le_kvset, true))
            need_finish = true;

    if (need_finish)
        tn_samp_update_finish(tn);

    tree->ct_samp.r_alen += tn->tn_samp.r_alen - orig.r_alen;
    tree->ct_samp.r_wlen += tn->tn_samp.r_wlen - orig.r_wlen;
    tree->ct_samp.i_alen += tn->tn_samp.i_alen - orig.i_alen;
    tree->ct_samp.l_alen += tn->tn_samp.l_alen - orig.l_alen;
    tree->ct_samp.l_good += tn->tn_samp.l_good - orig.l_good;
}

/* This function must be serialized with other cn_tree_samp_* functions.
 * It is used for ingest from c0 into root node and for ingesting
 * into children after spill operations.
 */
static void
cn_tree_samp_update_ingest(struct cn_tree *tree, struct cn_tree_node *tn)
{
    struct cn_samp_stats     orig;
    struct kvset_list_entry *le;

    orig = tn->tn_samp;

    le = list_first_entry_or_null(&tn->tn_kvset_list, typeof(*le), le_link);
    if (!le)
        return;

    orig = tn->tn_samp;

    if (tn_samp_update_incr(tn, le->le_kvset, false))
        tn_samp_update_finish(tn);

    tree->ct_samp.r_alen += tn->tn_samp.r_alen - orig.r_alen;
    tree->ct_samp.r_wlen += tn->tn_samp.r_wlen - orig.r_wlen;
    tree->ct_samp.i_alen += tn->tn_samp.i_alen - orig.i_alen;
    tree->ct_samp.l_alen += tn->tn_samp.l_alen - orig.l_alen;
    tree->ct_samp.l_good += tn->tn_samp.l_good - orig.l_good;
}

/* This function must be serialized with other cn_tree_samp_* functions. */
static void
cn_tree_samp_update_spill(struct cn_tree *tree, struct cn_tree_node *tn)
{
    struct cn_tree_node *leaf;

    /* A spill is esentially a compaction with an ingest into each child */

    assert(tn == tree->ct_root);

    cn_tree_samp_update_compact(tree, tn);

    cn_tree_foreach_leaf(leaf, tree) {
        cn_tree_samp_update_ingest(tree, leaf);
    }
}

/* This function must be serialized with other cn_tree_samp_* functions. */
void
cn_tree_samp_init(struct cn_tree *tree)
{
    struct cn_tree_node *tn;

    /* cn_tree_samp_update_compact() does a full recomputation
     * of samp stats, so use it to initalize tree samp stats.
     */
    memset(&tree->ct_samp, 0, sizeof(tree->ct_samp));

    cn_tree_foreach_node(tn, tree) {
        cn_tree_samp_update_compact(tree, tn);
    }
}

/* This function must be serialized with other cn_tree_samp_* functions
 * if a consistent set of stats is desired.
 */
void
cn_tree_samp(const struct cn_tree *tree, struct cn_samp_stats *s_out)

{
    *s_out = tree->ct_samp;
}

struct cn_tree_node *
cn_tree_find_node(struct cn_tree *tree, uint64_t nodeid)
{
    struct cn_tree_node *node;

    cn_tree_foreach_node(node, tree) {
        if (node->tn_nodeid == nodeid)
            break;
    }

    return node;
}

/**
 * cn_tree_insert_kvset - add kvset to tree during initialization
 * @tree:  tree under construction
 * @kvset: new kvset to add to tree
 * @nodeid: node ID
 *
 * This function is used during initialization to insert a kvset
 * into the correct node of the cn tree.
 *
 * NOTE: It is not intended to be used to update a node after compaction or
 * ingest operations.
 */
merr_t
cn_tree_insert_kvset(struct cn_tree *tree, struct kvset *kvset, uint64_t nodeid)
{
    struct cn_tree_node *node;

    assert(tree->ct_root == list_first_entry(&tree->ct_nodes, typeof(*node), tn_link));

    node = cn_tree_find_node(tree, nodeid);
    if (!node) {
        assert(0);
        return merr(EBUG);
    }

    return cn_node_insert_kvset(node, kvset);
}

merr_t
cn_node_insert_kvset(struct cn_tree_node *node, struct kvset *kvset)
{
    struct list_head *head;
    u64 dgen = kvset_get_dgen(kvset);

    list_for_each (head, &node->tn_kvset_list) {
        struct kvset_list_entry *entry;

        entry = list_entry(head, typeof(*entry), le_link);
        if (dgen > kvset_get_dgen(entry->le_kvset))
            break;
        assert(dgen != kvset_get_dgen(entry->le_kvset));
    }

    kvset_list_add_tail(kvset, head);

    return 0;
}

static void
kvset_view_free(void *arg)
{
    struct kvset_view *v = arg;

    if (v->kvset)
        kvset_put_ref(v->kvset);
}

void
cn_tree_view_destroy(struct table *view)
{
    table_apply(view, kvset_view_free);
    table_destroy(view);
}

merr_t
cn_tree_view_create(struct cn *cn, struct table **view_out)
{
    struct table *           view;
    struct cn_tree_node *    node;
    void *                   lock;
    struct cn_tree *         tree = cn_get_tree(cn);
    merr_t                   err = 0;
    uint nodecnt;

    nodecnt = (128 * 1024) / sizeof(struct kvset_view);

    view = table_create(nodecnt, sizeof(struct kvset_view), false);
    if (ev(!view))
        return merr(ENOMEM);

    rmlock_rlock(&tree->ct_lock, &lock);
    nodecnt = 0;

    cn_tree_foreach_node(node, tree) {
        struct kvset_list_entry *le;
        struct kvset_view *s;

        /* create an entry for the node */
        s = table_append(view);
        if (ev(!s)) {
            err = merr(ENOMEM);
            break;
        }

        s->kvset = NULL;
        s->nodeid = node->tn_nodeid;
        s->eklen = 0;

        if (node->tn_route_node)
            route_node_keycpy(node->tn_route_node, s->ekbuf, sizeof(s->ekbuf), &s->eklen);

        list_for_each_entry(le, &node->tn_kvset_list, le_link) {
            struct kvset *kvset = le->le_kvset;

            s = table_append(view);
            if (ev(!s)) {
                err = merr(ENOMEM);
                break;
            }

            kvset_get_ref(kvset);
            s->kvset = kvset;
            s->nodeid = kvset_get_nodeid(kvset);
            s->eklen = 0;

            assert(s->nodeid == node->tn_nodeid);
        }

        if (err)
            break;

        if ((nodecnt++ % 16) == 0)
            rmlock_yield(&tree->ct_lock, &lock);
    }
    rmlock_runlock(lock);

    if (err) {
        cn_tree_view_destroy(view);
        view = NULL;
    }

    *view_out = view;

    return err;
}

void
cn_tree_preorder_walk(
    struct cn_tree *          tree,
    enum kvset_order          kvset_order,
    cn_tree_walk_callback_fn *callback,
    void *                    callback_rock)
{
    struct cn_tree_node *    node;
    struct kvset_list_entry *le;
    void *                   lock;
    bool                     stop;

    rmlock_rlock(&tree->ct_lock, &lock);
    stop = false;

    cn_tree_foreach_node(node, tree) {
        bool empty_node = true;

        if (kvset_order == KVSET_ORDER_NEWEST_FIRST) {

            /* newest first ==> head to tail */
            list_for_each_entry (le, &node->tn_kvset_list, le_link) {
                empty_node = false;
                stop = callback(callback_rock, tree, node, le->le_kvset);
                if (stop)
                    goto unlock;
            }
        } else {
            /* oldest first ==> tail to head */
            list_for_each_entry_reverse (le, &node->tn_kvset_list, le_link) {
                empty_node = false;
                stop = callback(callback_rock, tree, node, le->le_kvset);
                if (stop)
                    goto unlock;
            }
        }

        /* end of node */
        if (!empty_node) {
            stop = callback(callback_rock, tree, node, NULL);
            if (stop)
                goto unlock;
        }
    }

unlock:
    if (!stop) {
        /* end of tree */
        callback(callback_rock, tree, NULL, NULL);
    }

    rmlock_runlock(lock);
}

struct cn_tree_node *
cn_tree_node_lookup(struct cn_tree *tree, const void *key, uint keylen)
{
    struct route_node *node;

    assert(tree && key);

    node = route_map_lookup(tree->ct_route_map, key, keylen);
    if (!node)
        return NULL;

    assert(node->rtn_tnode);

    return node->rtn_tnode;
}

struct route_node *
cn_tree_route_get(struct cn_tree *tree, const void *key, uint keylen)
{
    struct route_node *node;
    void *lock;

    assert(tree && key);

    rmlock_rlock(&tree->ct_lock, &lock);
    node = route_map_lookup(tree->ct_route_map, key, keylen);
    rmlock_runlock(lock);

    return node;
}

/**
 * cn_tree_lookup() - search cn tree for a key
 * @tree: cn tree
 * @pc:   perf counters
 * @kt:   key to search for
 * @seq:  view sequence number
 * @res:  (output) result (found value, found tomb, or not found)
 * @kbuf: (output) key if this is a prefix probe
 * @vbuf: (output) value if result @res == %FOUND_VAL or %FOUND_MULTIPLE
 *
 *
 * The following table shows the how the search descends the tree for
 * non-suffixed trees.
 *
 *   is tree     kt->kt_len vs
 *   a prefix    vs
 *   tree?       tree's pfx_len          descend by hash of:
 *   --------    -----------------       -------------------
 *     no        n/a                 ==> full key
 *     yes       kt_len <  pfx_len   ==> full key [1]
 *     yes       kt_len == pfx_len   ==> full key [2]
 *     yes       kt_len >  pfx_len   ==> prefix of key, then full key [3]
 *
 * Notes:
 *  [1]: Keys that are shorter than tree's prefix len are always
 *       stored by hash of full key.
 *
 *  [2]: Keys whose length is equal to the tree's prefix len can use
 *       the prefix hash or the full hash logic.  cn_tree_lookup() uses
 *       the full hash logic to take advantage of the pre-computed hash
 *       in @kt->kt_hash.
 *
 *  [3]: Descend by prefix until a certain depth, then switch to
 *       descend by full key (spill logic, of course, must use same
 *       logic).
 *
 * If the tree is suffixed,
 *
 *  [1]: Keys have to be at least (pfx_len + sfx_len) bytes long.
 *
 *  [2]: A full key hash is replaced with a hash over (keylen - sfx_len) bytes
 *       of the key.
 */
merr_t
cn_tree_lookup(
    struct cn_tree *     tree,
    struct perfc_set *   pc,
    struct kvs_ktuple *  kt,
    u64                  seq,
    enum key_lookup_res *res,
    struct query_ctx *   qctx,
    struct kvs_buf *     kbuf,
    struct kvs_buf *     vbuf)
{
    struct cn_tree_node *    node;
    struct key_disc          kdisc;
    void *                   lock;
    merr_t                   err;
    uint                     pc_nkvset;
    uint                     pc_depth;
    enum kvdb_perfc_sidx_cnget pc_cidx;
    u64                      pc_start;
    void *                   wbti;

    __builtin_prefetch(tree);

    *res = NOT_FOUND;
    err = 0;

    pc_cidx = PERFC_LT_CNGET_GET_L5 + 1;
    pc_depth = pc_nkvset = 0;

    pc_start = perfc_lat_startu(pc, PERFC_LT_CNGET_GET);
    if (pc_start > 0) {
        if (perfc_ison(pc, PERFC_LT_CNGET_GET_L0))
            pc_cidx = PERFC_LT_CNGET_GET_L0;
    }

    wbti = NULL;
    if (qctx->qtype == QUERY_PROBE_PFX) {
        err = kvset_wbti_alloc(&wbti);
        if (ev(err))
            return err;
    }

    key_disc_init(kt->kt_data, kt->kt_len, &kdisc);

    rmlock_rlock(&tree->ct_lock, &lock);
    node = tree->ct_root;

    while (node) {
        struct kvset_list_entry *le;

        /* Search kvsets from newest to oldest (head to tail).
         * If an error occurs or a key is found, return immediately.
         */
        list_for_each_entry (le, &node->tn_kvset_list, le_link) {
            struct kvset *kvset;

            kvset = le->le_kvset;
            ++pc_nkvset;

            switch (qctx->qtype) {
                case QUERY_GET:
                    err = kvset_lookup(kvset, kt, &kdisc, seq, res, vbuf);
                    if (err || *res != NOT_FOUND) {
                        rmlock_runlock(lock);

                        if (pc_cidx < PERFC_LT_CNGET_GET_L5 + 1)
                            perfc_lat_record(pc, pc_cidx, pc_start);
                        goto done;
                    }
                    break;

                case QUERY_PROBE_PFX:
                    err = kvset_pfx_lookup(kvset, kt, &kdisc, seq, res, wbti, kbuf, vbuf, qctx);
                    if (err || qctx->seen > 1 || *res == FOUND_PTMB) {
                        rmlock_runlock(lock);

                        ev(err);
                        goto done;
                    }
                    break;
            }
        }

        if (node != tree->ct_root)
            break;

        node = cn_tree_node_lookup(tree, kt->kt_data, kt->kt_len);

        ++pc_depth;
        ++pc_cidx;
    }
    rmlock_runlock(lock);

done:
    if (wbti) {
        perfc_lat_record(pc, PERFC_LT_CNGET_PROBE_PFX, pc_start);
        kvset_wbti_free(wbti);
    } else {
        if (pc_start > 0) {
            uint pc_cidx_lt = (*res == NOT_FOUND) ? PERFC_LT_CNGET_MISS : PERFC_LT_CNGET_GET;

            perfc_lat_record(pc, pc_cidx_lt, pc_start);
            perfc_rec_sample(pc, PERFC_DI_CNGET_DEPTH, pc_depth);
            perfc_rec_sample(pc, PERFC_DI_CNGET_NKVSET, pc_nkvset);
        }
    }

    perfc_inc(pc, *res);

    return err;
}

u64
cn_tree_initial_dgen(const struct cn_tree *tree)
{
    return tree->ct_dgen_init;
}

void
cn_tree_set_initial_dgen(struct cn_tree *tree, u64 dgen)
{
    tree->ct_dgen_init = dgen;
}

bool
cn_tree_is_capped(const struct cn_tree *tree)
{
    return cn_is_capped(tree->cn);
}

/* returns true if token acquired */
bool
cn_node_comp_token_get(struct cn_tree_node *tn)
{
    return atomic_cas(&tn->tn_compacting, 0, 1);
}

void
cn_node_comp_token_put(struct cn_tree_node *tn)
{
    bool b HSE_MAYBE_UNUSED;

    b = atomic_cas(&tn->tn_compacting, 1, 0);
    assert(b);
}

static void
cn_comp_release(struct cn_compaction_work *w)
{
    assert(w->cw_node);

    /* If this work is on the concurrent spill list then it must also
     * be at the head of the list.  If not, it means that the caller
     * applied a spill operation out-of-order such that a reader can
     * now read an old/stale key/value when it should have read a
     * newer one, meaning the kvdb is corrupted.
     */
    if (w->cw_rspill_conc) {
        struct cn_compaction_work *tmp HSE_MAYBE_UNUSED;

        mutex_lock(&w->cw_node->tn_rspills_lock);
        tmp = list_first_entry_or_null(&w->cw_node->tn_rspills, typeof(*tmp), cw_rspill_link);
        assert(tmp == w);
        list_del_init(&w->cw_rspill_link);
        mutex_unlock(&w->cw_node->tn_rspills_lock);
    }

    if (w->cw_err) {
        struct kvset_list_entry *le;
        uint kx;

        /* unmark input kvsets */
        le = w->cw_mark;
        for (kx = 0; kx < w->cw_kvset_cnt; kx++) {
            assert(le);
            assert(kvset_get_workid(le->le_kvset) != 0);
            kvset_set_workid(le->le_kvset, 0);
            le = list_prev_entry(le, le_link);
        }
    }

    if (w->cw_have_token)
        cn_node_comp_token_put(w->cw_node);

    perfc_inc(w->cw_pc, PERFC_BA_CNCOMP_FINISH);

    if (HSE_UNLIKELY(!w->cw_completion)) {
        free(w);
        return;
    }

    /* After this function returns the job will be disassociated
     * from its thread and hence becomes a zombie.  Do not touch
     * *w afterward as it may have already been freed.
     */
    w->cw_completion(w);
}

/**
 * cn_tree_capped_evict() - evict unneeded vblock pages
 * @tree:   cn_tree pointer
 * @first:  ptr to youngest kvset list entry
 * @last:   ptr to oldest kvset list entry
 *
 * This function attempts to identify pages in RAM from vblocks in a
 * capped kvs that are unlikely to be needed and advises the kernel
 * of their suitability for eviction.
 *
 * It scans the list from oldest to youngest kvset looking for kvsets
 * that have expired.  It evicts at most one kvset per scan, and tries
 * to remember where it left off to minimize subsequent scans.
 *
 * Note that this function should only be called within the context
 * of cn_tree_capped_compact() which ensures that the list of kvsets
 * from first to last is not empty and will not be modified.
 */
static void
cn_tree_capped_evict(
    struct cn_tree *         tree,
    struct kvset_list_entry *first,
    struct kvset_list_entry *last)
{
    struct kvset_list_entry *prev;
    struct kvset *           kvset;
    u64                      now;
    u64                      ttl;
    u64                      ctime;

    now = get_time_ns();

    if (tree->ct_capped_ttl > now)
        return;

    if (tree->ct_capped_dgen > kvset_get_dgen(last->le_kvset))
        last = tree->ct_capped_le;

    ttl = tree->rp->capped_evict_ttl * NSEC_PER_SEC;
    kvset = last->le_kvset;

    ctime = kvset_ctime(kvset);
    if (ctime + ttl > now) {
        tree->ct_capped_ttl = ctime + ttl;
        return;
    }

    if (last != first) {
        prev = list_prev_entry(last, le_link);
        tree->ct_capped_dgen = kvset_get_dgen(prev->le_kvset);
        tree->ct_capped_ttl = kvset_ctime(prev->le_kvset) + ttl;
        tree->ct_capped_le = prev;
    }

    kvset_madvise_vmaps(kvset, MADV_DONTNEED);
}

/**
 * cn_tree_capped_compact() - compact a capped tree
 * @tree:   cn_tree pointer
 *
 * This function trims expired kvsets from the tail of the capped kvs.
 */
void
cn_tree_capped_compact(struct cn_tree *tree)
{
    struct kvset_list_entry *le, *next, *mark;
    struct kvset_list_entry *first, *last;
    struct cn_tree_node *    node;
    struct list_head *       head, retired;
    struct cndb_txn         *cndb_txn;

    u8     pt_key[sizeof(tree->ct_last_ptomb)];
    void * lock;
    merr_t err;
    u64    horizon;
    u64    pt_seq;
    uint   pt_len;
    uint   kvset_cnt;

    node = tree->ct_root;
    head = &node->tn_kvset_list;

    /* While holding the tree read lock we acquire the first and last
     * kvset list entries.  As long as we do not access first->prev
     * nor last->next we can safely iterate between them without
     * holding the tree lock.
     */
    rmlock_rlock(&tree->ct_lock, &lock);
    pt_seq = tree->ct_last_ptseq;
    pt_len = tree->ct_last_ptlen;
    memcpy(pt_key, tree->ct_last_ptomb, pt_len);

    first = list_first_entry(head, typeof(*first), le_link);
    last = list_last_entry(head, typeof(*last), le_link);
    rmlock_runlock(lock);

    if (ev(first == last))
        return;

    horizon = cn_get_seqno_horizon(tree->cn);
    if (horizon > pt_seq)
        horizon = pt_seq;

    kvset_cnt = 0;
    mark = NULL;

    /* Step 1: Identify the kvsets that can be retired.
     */
    for (le = last; le != first; le = list_prev_entry(le, le_link)) {
        const void *max_key = NULL;
        uint  max_klen;

        /* [HSE_REVISIT] mapi breaks initialization of max_key.
         */
        kvset_get_max_key(le->le_kvset, &max_key, &max_klen);

        if (max_key && (!pt_len || kvset_get_seqno_max(le->le_kvset) >= horizon ||
                        keycmp_prefix(pt_key, pt_len, max_key, max_klen) < 0))
            break;

        ++kvset_cnt;
        mark = le;
    }

    perfc_set(cn_pc_capped_get(tree->cn), PERFC_BA_CNCAPPED_PTSEQ, pt_seq);

    if (!mark)
        goto err_out;

    err = cndb_record_txstart(tree->cndb, 0, CNDB_INVAL_INGESTID, CNDB_INVAL_HORIZON, 0,
                              (u16)kvset_cnt, &cndb_txn);
    if (ev(err))
        goto err_out;

    /* Step 2: Log kvset delete records.
     * Don't need to hold a lock because this is the only thread deleting
     * kvsets from cn and we are sure that there are at least kvset_cnt
     * kvsets in the node.
     */
    le = mark;
    while (1) {
        err = kvset_delete_log_record(le->le_kvset, cndb_txn);
        if (ev(err) || le == last)
            break;

        le = list_next_entry(le, le_link);
    }

    if (ev(err)) {
        cndb_record_nak(tree->cndb, cndb_txn);
        goto err_out;
    }

    /* Step 3: Remove retired kvsets from node list.
     */
    rmlock_wlock(&tree->ct_lock);
    list_trim(&retired, head, &mark->le_link);
    cn_tree_samp_update_compact(tree, node);
    rmlock_wunlock(&tree->ct_lock);

    /* Step 4: Delete retired kvsets outside the tree write lock.
     */
    list_for_each_entry_safe(le, next, &retired, le_link) {
        kvset_mark_mblocks_for_delete(le->le_kvset, false);
        kvset_put_ref(le->le_kvset);
    }

    return;

err_out:
    cn_tree_capped_evict(tree, first, last);
    return;
}

merr_t
cn_tree_prepare_compaction(struct cn_compaction_work *w)
{
    struct kvset_mblocks    *outs = 0;
    struct kvset_vblk_map    vbm = {};
    struct workqueue_struct *vra_wq;
    struct cn_tree_node     *node = w->cw_node;
    struct kvset_list_entry *le;
    struct kv_iterator     **ins = NULL;
    merr_t err = 0;
    size_t outsz = 0;
    u32 n_outs;
    u32 fanout;
    uint i;
    const bool kcompact = (w->cw_action == CN_ACTION_COMPACT_K);
    const bool split = (w->cw_action == CN_ACTION_SPLIT);

    fanout = w->cw_tree->ct_fanout;
    n_outs = fanout;

    /* If we are k/kv-compacting, we only have a single output.
     *
     * Node split creates at most twice the number of kvsets as the source node (n_outs)
     * The two output nodes for split are stored in cw_split.nodev[]
     */
    if (split) {
        if (cn_ns_kvsets(&w->cw_node->tn_ns) != w->cw_kvset_cnt)
            return merr(EBUG);

        n_outs = 2 * w->cw_kvset_cnt;
    } else {
        if (kcompact || w->cw_action == CN_ACTION_COMPACT_KV)
            n_outs = 1;

        ins = calloc(w->cw_kvset_cnt, sizeof(*ins));
        if (!ins)
            return merr(ENOMEM);

        outsz = sizeof(w->cw_output_nodev[0]);
    }

    outsz += (sizeof(*outs) + sizeof(*w->cw_kvsetidv));
    outs = calloc(n_outs, outsz);
    if (!outs) {
        err = merr(ENOMEM);
        goto err_exit;
    }

    w->cw_kvsetidv = (void *)(outs + n_outs);
    if (!split)
        w->cw_output_nodev = (void *)(w->cw_kvsetidv + n_outs);

    w->cw_vgmap = NULL;
    if (kcompact || split) {
        w->cw_vgmap = calloc(n_outs, sizeof(*w->cw_vgmap));
        if (!w->cw_vgmap) {
            err = merr(ENOMEM);
            goto err_exit;
        }

        if (split) {
            size_t sz = HSE_KVS_KEY_LEN_MAX +
                n_outs * (sizeof(*(w->cw_split.commit)) + sizeof(*(w->cw_split.dgen))) +
                w->cw_kvset_cnt * sizeof(*(w->cw_split.purge));

            w->cw_split.key = calloc(1, sz);
            if (!w->cw_split.key) {
                err = merr(ENOMEM);
                goto err_exit;
            }

            w->cw_split.commit = w->cw_split.key + HSE_KVS_KEY_LEN_MAX;
            w->cw_split.purge = w->cw_split.commit + n_outs;
            w->cw_split.dgen = (void *)(w->cw_split.purge + w->cw_kvset_cnt);
        }
    }

    vra_wq = cn_get_maint_wq(node->tn_tree->cn);

    /*
     * Create one iterator for each input kvset.  The list 'ins' must be
     * ordered such that 'ins[i]' is newer then 'ins[i+1]'.  We walk the
     * list from old to new, so the 'ins' list is populated from
     * 'ins[n-1]' to 'ins[0]'.
     *
     * The kvset list lock is not required because the kvsets we are
     * looking at are adacent in the list and are marked (with a workid).
     * Just be careful not to try to iterate outside the range of marked
     * kvsets.
     *
     * Node splits do not need input iterators because there's no merge loop.
     */
    for (i = 0, le = w->cw_mark; !split && i < w->cw_kvset_cnt;
         i++, le = list_prev_entry(le, le_link)) {

        struct kv_iterator **iter = &ins[w->cw_kvset_cnt - 1 - i];

        if (i == 0)
            assert(kvset_get_dgen(le->le_kvset) == w->cw_dgen_lo);
        if (i == w->cw_kvset_cnt - 1)
            assert(kvset_get_dgen(le->le_kvset) == w->cw_dgen_hi);

        err = kvset_iter_create(
            le->le_kvset, w->cw_io_workq, vra_wq, w->cw_pc, w->cw_iter_flags, iter);
        if (ev(err))
            goto err_exit;

        kvset_iter_set_stats(*iter, &w->cw_stats);
    }

    /* k-compaction keeps all the vblocks from the source kvsets
     * vbm_blkv[0] is the id of the first vblock of the newest kvset
     * vbm_blkv[n] is the id of the last vblock of the oldest kvset
     */
    if (kcompact) {
        err = kvset_keep_vblocks(&vbm, w->cw_vgmap, ins, w->cw_kvset_cnt);
        if (ev(err))
            goto err_exit;
    }

    w->cw_inputv = ins;
    w->cw_outc = n_outs;
    w->cw_outv = outs;
    w->cw_vbmap = vbm;

    /* Enable dropping of tombstones in merge logic if 'mark' is
     * the oldest kvset in the node and we're not spilling.
     */
    w->cw_drop_tombs = (w->cw_action != CN_ACTION_SPILL) &&
        (w->cw_mark == list_last_entry(&node->tn_kvset_list, struct kvset_list_entry, le_link));

    return 0;

err_exit:
    if (ins) {
        for (i = 0; i < w->cw_kvset_cnt; i++)
            if (ins[i])
                ins[i]->kvi_ops->kvi_release(ins[i]);
        free(ins);
        free(vbm.vbm_blkv);
        if (w->cw_vgmap) {
            if (kcompact)
                vgmap_free(w->cw_vgmap[0]); /* one output kvset for k-compact */
            else if (split)
                free(w->cw_split.key);
            free(w->cw_vgmap);
        }
    }
    free(outs);

    return err;
}

/*----------------------------------------------------------------
 *
 * SECTION: Cn Tree Compaction (k-compaction, kv-compaction, spill)
 *
 * The following annotated call graph of functions in this section provides an
 * overview of the code structure.  The compaction scheduler (csched) submits
 * jobs to the short term scheduler (STS).  Callbacks from STS land in
 * cn_comp(), which is the top of the call graph shown here.  Underscores are
 * used to preserve whitespace.
 *
 *    cn_comp()
 *    ___ cn_comp_compact()   // merge kvsets into kvsets
 *    _______ cn_spill()      //   for spill and kv-compact
 *    _______ cn_kcompact()   //   for k-compact
 *    _______ cn_split()      //   for node split
 *    ___ cn_comp_finish()    // commit, update and cleaup
 *    _______ cn_comp_commit()                 // create kvsets and commit to cNDB
 *    ____________cn_comp_update_spill()       //   update cn tree for spill
 *    ____________cn_comp_update_kvcompact()   //   update cn tree for kv-compact
 *    ____________cn_comp_update_split()       //   update cn tree for node split
 *    _______ cn_comp_cleanup()
 *    _______ cn_comp_release()
 *    ___________ w->cw_completion()           // completion callback
 *
 */

/**
 * cn_comp_update_kvcompact() - Update tree after k-compact and kv-compact
 * See section comment for more info.
 */
static void
cn_comp_update_kvcompact(struct cn_compaction_work *work, struct kvset *new_kvset)
{
    struct cn_tree *         tree = work->cw_tree;
    struct kvset_list_entry *le, *tmp;
    struct list_head         retired_kvsets;
    uint                     i;

    if (ev(work->cw_err))
        return;

    INIT_LIST_HEAD(&retired_kvsets);
    assert(work->cw_dgen_lo == kvset_get_workid(work->cw_mark->le_kvset));

    rmlock_wlock(&tree->ct_lock);
    {
        assert(!list_empty(&work->cw_node->tn_kvset_list));
        le = work->cw_mark;
        for (i = 0; i < work->cw_kvset_cnt; i++) {
            tmp = list_prev_entry(le, le_link);
            list_del(&le->le_link);
            list_add(&le->le_link, &retired_kvsets);
            le = tmp;
        }

        if (new_kvset) {
            kvset_list_add(new_kvset, &le->le_link);
            work->cw_node->tn_cgen++;
        }
    }

    cn_tree_samp(tree, &work->cw_samp_pre);

    cn_tree_samp_update_compact(tree, work->cw_node);

    cn_tree_samp(tree, &work->cw_samp_post);

    atomic_sub_rel(&work->cw_node->tn_busycnt, (1u << 16) + work->cw_kvset_cnt);
    rmlock_wunlock(&tree->ct_lock);

    /* Delete retired kvsets. */
    list_for_each_entry_safe(le, tmp, &retired_kvsets, le_link) {

        assert(kvset_get_dgen(le->le_kvset) >= work->cw_dgen_lo);
        assert(kvset_get_dgen(le->le_kvset) <= work->cw_dgen_hi);

        kvset_mark_mblocks_for_delete(le->le_kvset, work->cw_keep_vblks);
        kvset_put_ref(le->le_kvset);
    }
}

/**
 * cn_comp_update_spill() - update tree after spill operation
 * See section comment for more info.
 */
static void
cn_comp_update_spill(struct cn_compaction_work *work, struct kvset **kvsets)
{
    struct cn_tree *         tree = work->cw_tree;
    struct cn_tree_node *    pnode = work->cw_node;
    struct kvset_list_entry *le, *tmp;
    struct list_head         retired_kvsets;
    struct cn_tree_node *    node;

    if (ev(work->cw_err))
        return;

    INIT_LIST_HEAD(&retired_kvsets);

    rmlock_wlock(&tree->ct_lock);
    {
        for (uint i = 0; i < work->cw_outc; i++) {
            if (kvsets[i]) {
                node = work->cw_output_nodev[i];
                assert(node);
                kvset_list_add(kvsets[i], &node->tn_kvset_list);
                node->tn_cgen++;
            }
        }

        /* Advance the change generation on the spill source node
         * to ensure it is reevaluated by csched/sp3_dirty_node().
         */
        pnode->tn_cgen++;

        /* Move old kvsets from parent node to retired list.
         * Asserts:
         * - Each input kvset just spilled must still be on pnode's kvset list.
         * - The dgen of the oldest input kvset must match work struct dgen_lo
         *   (i.e., concurrent spills from a node must be committed in order).
         */
        for (uint i = 0; i < work->cw_kvset_cnt; i++) {
            assert(!list_empty(&pnode->tn_kvset_list));
            le = list_last_entry(&pnode->tn_kvset_list, struct kvset_list_entry, le_link);
            assert(i > 0 || work->cw_dgen_lo == kvset_get_dgen(le->le_kvset));
            list_del(&le->le_link);
            list_add(&le->le_link, &retired_kvsets);
        }

        cn_tree_samp(tree, &work->cw_samp_pre);

        cn_tree_samp_update_spill(tree, pnode);

        cn_tree_samp(tree, &work->cw_samp_post);

        atomic_sub_rel(&pnode->tn_busycnt, (1u << 16) + work->cw_kvset_cnt);
    }
    rmlock_wunlock(&tree->ct_lock);

    /* Delete old kvsets. */
    list_for_each_entry_safe(le, tmp, &retired_kvsets, le_link) {
        kvset_mark_mblocks_for_delete(le->le_kvset, false);
        kvset_put_ref(le->le_kvset);
    }
}

/**
 * cn_comp_update_split() - update tree after a node split operation
 */
static merr_t
cn_comp_update_split(
    struct cn_compaction_work *w,
    struct kvset *const       *kvsets,
    const uint64_t             nodeidv[static 2])
{
    struct cn_tree *tree = w->cw_tree;
    struct kvset_list_entry *le, *tmp;
    struct list_head retired_kvsets;
    struct cn_tree_node *left = NULL, *right = w->cw_node;
    char rekey[HSE_KVS_KEY_LEN_MAX];
    uint k = 0, reklen;
    merr_t err = 0;

    if (ev(w->cw_err))
        return w->cw_err;

    INIT_LIST_HEAD(&retired_kvsets);

    /* Allocate a new left node and add the split output kvsets on the left to this node.
     * This need not be done under the tree lock as this new node is not published yet.
     */
    if (nodeidv[0] != CN_TREE_INVALID_NODEID) {
        left = cn_node_alloc(tree, nodeidv[0]);
        if (!left)
            return merr(ENOMEM);

        for (k = 0; k < w->cw_kvset_cnt; k++) {
            if (kvsets[k])
                kvset_list_add_tail(kvsets[k], &left->tn_kvset_list);
        }

        w->cw_split.nodev[0] = left;
    }

    if (nodeidv[1] != CN_TREE_INVALID_NODEID) {
        /* The 'right' node is protected by an exclusive compaction token, so the max key cannot
         * change while a node split is in progress
         */
        cn_tree_node_get_max_key(right, rekey, sizeof(rekey), &reklen);
    }

    rmlock_wlock(&tree->ct_lock);

    do {
        /* Move all the source kvsets from the 'right' node to the retired list.
         */
        list_splice(&right->tn_kvset_list, &retired_kvsets);
        INIT_LIST_HEAD(&right->tn_kvset_list);

        /* Add the right half of the split kvsets to the 'right' node.
         */
        if (nodeidv[1] != CN_TREE_INVALID_NODEID) {
            right->tn_nodeid = nodeidv[1];

            assert(list_empty(&right->tn_kvset_list));

            for (k = w->cw_kvset_cnt; k < w->cw_outc; k++) {
                if (kvsets[k])
                    kvset_list_add_tail(kvsets[k], &right->tn_kvset_list);
            }

            /* The last node in the route map contains all keys that are greater than the
             * penultimate node. Under rare circumstances the split key chosen for the last
             * node can be lexicographically greater than its edge key. The below logic
             * detects this situation and updates the edge key of the right node to its
             * max key at the time of split.
             */
            if (route_node_islast(right->tn_route_node)) {
                int rc = route_node_keycmp(right->tn_route_node, w->cw_split.key, w->cw_split.klen);
                if (ev(rc <= 0)) {
                    err = route_node_key_modify(tree->ct_route_map, right->tn_route_node,
                                                rekey, reklen);
                    if (err)
                        break;
                }
            }
            assert(route_node_keycmp(right->tn_route_node, w->cw_split.key, w->cw_split.klen) > 0);

            w->cw_split.nodev[1] = right;
        }

        /* Update route map with the left edge and add the new left node to the cN tree list.
         * The right node is already part of the cn tree list.
         */
        if (left) {
            left->tn_route_node = route_map_insert(tree->ct_route_map, left,
                                                   w->cw_split.key, w->cw_split.klen);
            if (!left->tn_route_node) {
                err = merr(ENOMEM);
                break;
            }

            list_add_tail(&left->tn_link, &tree->ct_nodes);
        }

        /* Update samp stats
         */
        for (int i = 0; i < 2 && w->cw_split.nodev[i]; i++) {
            cn_tree_samp(tree, &w->cw_samp_pre);
            cn_tree_samp_update_compact(tree, w->cw_split.nodev[i]);
            cn_tree_samp(tree, &w->cw_samp_post);
        }

        atomic_sub_rel(&right->tn_busycnt, (1u << 16) + w->cw_kvset_cnt);
    } while (0);

    rmlock_wunlock(&tree->ct_lock);

    if (err) {
        cn_node_free(left);

        return err;
    }

    /* Delete retired kvsets
     */
    k = 0;
    list_for_each_entry_safe(le, tmp, &retired_kvsets, le_link) {
        struct kvset *ks = le->le_kvset;

        kvset_purge_blklist_add(ks, &w->cw_split.purge[k]);
        blk_list_free(&w->cw_split.purge[k]);

        kvset_mark_mbset_for_delete(ks, false);
        kvset_put_ref(ks);
        k++;
    }

    return 0;
}

static bool
check_valid_kvsets(const struct cn_compaction_work *w, uint start, uint end)
{
    for (uint i = start; i < end; i++) {
        if (w->cw_kvsetidv[i] != 0)
            return true;
    }

    return false;
}

static void
cn_split_nodeids_get(const struct cn_compaction_work *w, uint64_t nodeidv[static 2])
{
    for (int i = 0; i < 2; i++) {
        uint start, end;

        if (i == 0) {
            start = 0;
            end = w->cw_kvset_cnt;
        } else {
            start = w->cw_kvset_cnt;
            end = w->cw_outc;
        }

        nodeidv[i] = check_valid_kvsets(w, start, end) ?
            cndb_nodeid_mint(cn_tree_get_cndb(w->cw_tree)) : CN_TREE_INVALID_NODEID;
    }
}

/**
 * cn_comp_commit() - commit compaction operation to cndb log
 * See section comment for more info.
 */
static void
cn_comp_commit(struct cn_compaction_work *w)
{
    struct kvset **kvsets = 0;
    struct mbset ***vecs = 0;
    uint *cnts = 0;
    uint i, alloc_len;
    const bool spill = (w->cw_action == CN_ACTION_SPILL);
    const bool split = (w->cw_action == CN_ACTION_SPLIT);
    const bool kcompact = (w->cw_action == CN_ACTION_COMPACT_K);
    const bool use_mbsets = kcompact;
    bool skip_commit = false, txn_nak = false;
    void **cookiev = 0;
    uint64_t nodeidv[2];

    struct kvdb_health *hp = w->cw_tree->ct_kvdb_health;
    struct kvset_list_entry *le;

    if (ev(w->cw_err))
        goto done;

    assert(w->cw_outc);

    /* if k-compaction and no kblocks, then force keepv to false. */
    if (kcompact && w->cw_outv[0].kblks.n_blks == 0) {
        skip_commit = true;
        w->cw_keep_vblks = false;
    }

    if (!skip_commit) {
        cookiev = calloc(w->cw_outc, sizeof(*cookiev));
        if (ev(!cookiev)) {
            w->cw_err = merr(ENOMEM);
            kvdb_health_event(hp, KVDB_HEALTH_FLAG_NOMEM, w->cw_err);
            goto done;
        }
    }

    alloc_len = sizeof(*kvsets) * w->cw_outc;
    if (use_mbsets) {
        /* For k-compaction, create new kvset with references to
         * mbsets from input kvsets instead of creating new mbsets.
         * We need extra allocations for this.
         */
        alloc_len += sizeof(*vecs) * w->cw_kvset_cnt;
        alloc_len += sizeof(*cnts) * w->cw_kvset_cnt;
    }

    kvsets = calloc(1, alloc_len);
    if (ev(!kvsets)) {
        w->cw_err = merr(ENOMEM);
        goto done;
    }

    if (use_mbsets) {
        struct kvset_list_entry *le;

        vecs = (void *)(kvsets + w->cw_outc);
        cnts = (void *)(vecs + w->cw_kvset_cnt);

        /* The kvset represented by vecs[i] must be newer than
         * the kvset represented by vecs[i+1] (that is, in same order
         * as the vector of iterators used in the compaction/merge
         * loops).
         */
        le = w->cw_mark;
        i = w->cw_kvset_cnt;
        while (i--) {
            vecs[i] = kvset_get_vbsetv(le->le_kvset, &cnts[i]);
            le = list_prev_entry(le, le_link);
        }
    }

    w->cw_err = cndb_record_txstart(w->cw_tree->cndb, 0, CNDB_INVAL_INGESTID, CNDB_INVAL_HORIZON,
                                    (u16)w->cw_outc, (u16)w->cw_kvset_cnt, &w->cw_cndb_txn);
    if (ev(w->cw_err)) {
        kvdb_health_error(hp, w->cw_err);
        goto done;
    }
    txn_nak = true;

    if (split)
        cn_split_nodeids_get(w, nodeidv);

    /* Log CNDB records for all kvsets before committing the mblocks.
     */
    for (i = 0; i < w->cw_outc; i++) {
        struct kvset_meta km = {};

        /* A k-compact with sufficient tombs could annihilate all keys,
         * in which case it will have no h or k blocks, but it may have
         * vblocks that need to be deleted.  In this case skip_commit
         * should be true.
         *
         * [HSE_REVISIT] Are there any other corner cases?
         */
        if (!w->cw_outv[i].hblk.bk_blkid) {
            assert(w->cw_outv[i].kblks.n_blks == 0);
            assert(skip_commit || w->cw_outv[i].vblks.n_blks == 0);
            continue;
        }

        km.km_dgen = w->cw_dgen_hi;
        km.km_vused = w->cw_outv[i].bl_vused;

        /* Lend hblk, kblk, and vblk lists to kvset_open().
         * Yes, the struct copy is a bit gross, but it works and
         * avoids unnecessary allocations of temporary lists.
         */
        km.km_hblk = w->cw_outv[i].hblk;
        km.km_kblk_list = w->cw_outv[i].kblks;
        km.km_vblk_list = w->cw_outv[i].vblks;

        km.km_comp_rule = w->cw_comp_rule;
        km.km_capped = cn_is_capped(w->cw_tree->cn);
        km.km_restored = false;

        if (spill) {
            struct cn_tree_node *node = w->cw_output_nodev[i];

            assert(node);
            km.km_compc = 0;
            km.km_nodeid = node->tn_nodeid;

            /* Monotonic loads tend to create very large kvsets.  If this
             * is the first kvset in the node and it appears to have either
             * a lot of keys or a large vlen, then seed it with a large compc
             * to defer it from being unnecessarily rewritten by node-length-
             * reduction and/or scatter-remediation jobs.
             */
            if (cn_ns_kvsets(&node->tn_ns) == 0) {
                if (w->cw_outv[i].kblks.n_blks > 2 || w->cw_outv[i].vblks.n_blks > 32)
                    km.km_compc += 7;
            }
        } else if (split) {
            km.km_compc = w->cw_compc;
            km.km_nodeid = nodeidv[i / w->cw_kvset_cnt];
            assert(km.km_nodeid != CN_TREE_INVALID_NODEID);
            km.km_dgen = w->cw_split.dgen[i];
        } else {
            struct kvset_list_entry *le = w->cw_mark;

            km.km_compc = w->cw_compc;
            km.km_nodeid = w->cw_node->tn_nodeid;

            /* If we're in the middle of a run then do not increment compc
             * if it would become greater than the next older kvset.
             */
            le = list_next_entry_or_null(le, le_link, &w->cw_node->tn_kvset_list);
            if (!le || w->cw_compc < kvset_get_compc(le->le_kvset))
                km.km_compc++;
        }

        /* CNDB: Log kvset add records.
         */
        w->cw_err = cndb_record_kvset_add(
                        w->cw_tree->cndb, w->cw_cndb_txn, w->cw_tree->cnid,
                        km.km_nodeid, &km, w->cw_kvsetidv[i], km.km_hblk.bk_blkid,
                        w->cw_outv[i].kblks.n_blks, (uint64_t *)w->cw_outv[i].kblks.blks,
                        w->cw_outv[i].vblks.n_blks, (uint64_t *)w->cw_outv[i].vblks.blks,
                        &cookiev[i]);

        if (ev(w->cw_err)) {
            kvdb_health_error(hp, w->cw_err);
            goto done;
        }

        if (split) {
            w->cw_err = commit_mblocks(w->cw_mp, &w->cw_split.commit[i]);
            if (!w->cw_err)
                blk_list_free(&w->cw_split.commit[i]);
        } else {
            w->cw_err = cn_mblocks_commit(w->cw_mp, 1, &w->cw_outv[i],
                                          kcompact ? CN_MUT_KCOMPACT : CN_MUT_OTHER);
        }
        if (ev(w->cw_err)) {
            kvdb_health_error(hp, w->cw_err);
            goto done;
        }

        if (use_mbsets) {
            w->cw_err = kvset_open2(w->cw_tree, w->cw_kvsetidv[i], &km,
                                      w->cw_kvset_cnt, cnts, vecs, &kvsets[i]);
        } else {
            w->cw_err = kvset_open(w->cw_tree, w->cw_kvsetidv[i], &km, &kvsets[i]);
        }

        if (ev(w->cw_err))
            goto done;
    }

    /* CNDB: Log kvset delete records.
     */
    for (i = 0, le = w->cw_mark; i < w->cw_kvset_cnt; i++) {
        assert(le);
        assert(w->cw_cndb_txn);

        w->cw_err = kvset_delete_log_record(le->le_kvset, w->cw_cndb_txn);
        if (ev(w->cw_err))
            goto done;

        le = list_prev_entry(le, le_link);
    }

    /* CNDB: Ack all the kvset add records.
     */
    for (i = 0; i < w->cw_outc; i++) {
        if (!w->cw_outv[i].hblk.bk_blkid)
            continue;

        w->cw_err = cndb_record_kvset_add_ack(w->cw_tree->cndb, w->cw_cndb_txn, cookiev[i]);
        if (ev(w->cw_err))
            goto done;
    }

    switch (w->cw_action) {
    case CN_ACTION_NONE:
    case CN_ACTION_END:
        break;

    case CN_ACTION_COMPACT_K:
    case CN_ACTION_COMPACT_KV:
        cn_comp_update_kvcompact(w, kvsets[0]);
        break;

    case CN_ACTION_SPILL:
        cn_comp_update_spill(w, kvsets);
        break;

    case CN_ACTION_SPLIT:
        w->cw_err = cn_comp_update_split(w, kvsets, nodeidv);
        break;
    }

done:
    if (w->cw_err && kvsets) {
        if (txn_nak)
            cndb_record_nak(w->cw_tree->cndb, w->cw_cndb_txn);

        for (i = 0; i < w->cw_outc; i++) {
            if (kvsets[i])
                kvset_put_ref(kvsets[i]);
        }
    }

    /* always free these ptrs */
    free(cookiev);
    free(kvsets);
}

/**
 * cn_comp_cleanup() - cleanup after compaction operation
 * See section comment for more info.
 */
static void
cn_comp_cleanup(struct cn_compaction_work *w)
{
    const bool kcompact = (w->cw_action == CN_ACTION_COMPACT_K);
    const bool split = (w->cw_action == CN_ACTION_SPLIT);

    if (HSE_UNLIKELY(w->cw_err)) {

        /* Failed spills cause node to become "wedged"  */
        if (ev(w->cw_rspill_conc && !w->cw_node->tn_rspills_wedged))
            w->cw_node->tn_rspills_wedged = 1;

        /* Log errors if debugging or if job was not canceled.
         * Canceled jobs are expected, so there's no need to log them
         * unless debugging.
         */
        if (!w->cw_canceled)
            log_errx("compaction error @@e: sts/job %u comp %s rule %s"
                     " cnid %lu nodeid %lu dgenlo %lu dgenhi %lu wedge %d",
                     w->cw_err,
                     sts_job_id_get(&w->cw_job),
                     cn_action2str(w->cw_action),
                     cn_comp_rule2str(w->cw_comp_rule),
                     cn_tree_get_cnid(w->cw_tree),
                     w->cw_node->tn_nodeid,
                     w->cw_dgen_lo,
                     w->cw_dgen_hi,
                     w->cw_node->tn_rspills_wedged);

        if (merr_errno(w->cw_err) == ENOSPC)
            w->cw_tree->ct_nospace = true;

        if (split) {
            for (uint i = 0; i < w->cw_outc && w->cw_split.commit; i++) {
                delete_mblocks(w->cw_mp, &w->cw_split.commit[i]);
                blk_list_free(&w->cw_split.commit[i]);
                if (i < w->cw_kvset_cnt) {
                    assert(w->cw_split.purge);
                    blk_list_free(&w->cw_split.purge[i]);
                }
            }
        } else if (w->cw_outv) {
            cn_mblocks_destroy(w->cw_mp, w->cw_outc, w->cw_outv, kcompact);
        }
    }

    free(w->cw_vbmap.vbm_blkv);

    if (w->cw_vgmap) {
        if (kcompact) {
            vgmap_free(w->cw_vgmap[0]); /* One output kvset for k-compact */
        } else if (split) {
            for (uint i = 0; i < w->cw_outc; i++)
                vgmap_free(w->cw_vgmap[i]);

            free(w->cw_split.key);
        }

        free(w->cw_vgmap);
    }

    free(w->cw_cookie);

    if (w->cw_outv) {
        for (uint i = 0; i < w->cw_outc; i++) {
            blk_list_free(&w->cw_outv[i].kblks);
            blk_list_free(&w->cw_outv[i].vblks);
        }
        free(w->cw_outv);
    }
}

/**
 * get_completed_spill() - reorder ingests into root node
 * See section comment for more info.
 */
static struct cn_compaction_work *
get_completed_spill(struct cn_tree_node *node)
{
    struct cn_compaction_work *w = 0;

    mutex_lock(&node->tn_rspills_lock);

    w = list_first_entry_or_null(&node->tn_rspills, typeof(*w), cw_rspill_link);
    if (!w)
        goto done;

    /* Punt if job on head of list is not done or another thread is already committing it. */
    if (!atomic_read(&w->cw_rspill_done) || atomic_read(&w->cw_rspill_commit_in_progress)) {
        w = 0;
        goto done;
    }

    /* Job on head of spill completion list is ready to be processed.
     * - Set "commit_in_progress" status, but leave on list until commit is done.
     * - If the node is wedged, it means an earlier job has failed, in
     *   which case we force failure on this job to prevent out of
     *   order completion.
     * - If the node is not wedged, and this job has failed then it
     *   will cause the node to be wedged, but this will be handled
     *   later to catch downstream errors.
     */

    atomic_set(&w->cw_rspill_commit_in_progress, 1);

    if (ev(node->tn_rspills_wedged && !w->cw_err)) {
        w->cw_err = merr(ESHUTDOWN);
        w->cw_canceled = true;
    }

done:
    mutex_unlock(&node->tn_rspills_lock);

    return w;
}

/**
 * cn_comp_compact() - perform the actual compaction operation
 * See section comment for more info.
*/
static void
cn_comp_compact(struct cn_compaction_work *w)
{
    struct kvdb_health *hp;
    merr_t err;

    if (ev(w->cw_err))
        return;

    w->cw_horizon = cn_get_seqno_horizon(w->cw_tree->cn);
    w->cw_cancel_request = cn_get_cancel(w->cw_tree->cn);

    perfc_inc(w->cw_pc, PERFC_BA_CNCOMP_START);

    cn_setname(w->cw_threadname);

    w->cw_t1_qtime = get_time_ns();

    hp = w->cw_tree->ct_kvdb_health;
    assert(hp);

    err = kvdb_health_check(hp, KVDB_HEALTH_FLAG_ALL);
    if (ev(err))
        goto err_exit;

    /* cn_tree_prepare_compaction() will initiate I/O
     * if ASYNCIO is enabled.
     */
    err = cn_tree_prepare_compaction(w);
    if (ev(err)) {
        kvdb_health_error(hp, err);
        goto err_exit;
    }

    w->cw_t2_prep = get_time_ns();

    /* cn_kcompact handles k-compaction, cn_spill handles spills
     * and kv-compaction. */
    w->cw_keep_vblks = (w->cw_action == CN_ACTION_COMPACT_K);

    switch (w->cw_action) {
    case CN_ACTION_NONE:
    case CN_ACTION_END:
        break;

    case CN_ACTION_COMPACT_K:
        err = cn_kcompact(w);
        break;

    case CN_ACTION_COMPACT_KV:
    case CN_ACTION_SPILL:
        err = cn_spill(w);
        break;

    case CN_ACTION_SPLIT:
        err = cn_split(w);
        break;
    }

    if (merr_errno(err) == ESHUTDOWN && atomic_read(w->cw_cancel_request))
        w->cw_canceled = true;

    /* defer status check until *after* cleanup */
    for (uint i = 0; i < w->cw_kvset_cnt && w->cw_inputv; i++) {
        if (w->cw_inputv[i])
            w->cw_inputv[i]->kvi_ops->kvi_release(w->cw_inputv[i]);
    }

    free(w->cw_inputv);

    if (ev(err)) {
        if (!w->cw_canceled)
            kvdb_health_error(hp, err);
        goto err_exit;
    }

    w->cw_t3_build = get_time_ns();
    w->cw_t4_commit = get_time_ns();

err_exit:
    w->cw_err = err;
    if (w->cw_canceled && !w->cw_err)
        w->cw_err = merr(ESHUTDOWN);
}

/**
 * cn_comp_finish() - finish a committed compaction operation
 * See section comment for more info.
 */
static void
cn_comp_finish(struct cn_compaction_work *w)
{
    cn_comp_commit(w);
    cn_comp_cleanup(w);
    cn_comp_release(w);
}

/**
 * cn_comp() - perform a cn tree compaction operation
 *
 * This function is invoked by short tern schedeler by way of callbacks
 * cn_comp_cancel_cb() and cn_comp_slice_cb().  See section comment for more
 * info.
 */
static void
cn_comp(struct cn_compaction_work *w)
{
    u64               tstart;
    struct cn        *cn = w->cw_tree->cn;
    struct perfc_set *pc = w->cw_pc;

    tstart = perfc_lat_start(pc);

    cn_comp_compact(w);

    /* Detach this job from the callback thread as we're about
     * to either hand it off to the monitor thread or leave it
     * on the rspill list for some other thread to finish.
     */
    sts_job_detach(&w->cw_job);

    /* Acquire a cn reference here to prevent cn from closing
     * before we finish updating the latency perf counter.
     * Do not touch *w after calling cn_comp_finish()
     * as it may have already been freed.
     */
    cn_ref_get(cn);
    if (w->cw_rspill_conc) {
        struct cn_tree_node *node;

        /* Mark this root spill as done.  Then process tn_rspill_list
         * to ensure concurrent root spills are completed in the
         * correct order.
         */
        atomic_set(&w->cw_rspill_done, 1);
        node = w->cw_node;
        while (NULL != (w = get_completed_spill(node)))
            cn_comp_finish(w);
    } else {
        /* Non-root spill (only one at a time per node). */
        cn_comp_finish(w);
        w = NULL;
    }

    perfc_lat_record(pc, PERFC_LT_CNCOMP_TOTAL, tstart);
    cn_ref_put(cn);
}

/**
 * cn_comp_slice_cb() - sts callback to run an sts job slice
 */
void
cn_comp_slice_cb(struct sts_job *job)
{
    struct cn_compaction_work *w = container_of(job, typeof(*w), cw_job);

    cn_comp(w);
}

/**
 * cn_tree_ingest_update() - Update the cn tree with the new kvset
 * @tree:  pointer to struct cn_tree.
 * @le:    kvset's link on the kvset list.
 * @ptomb: max ptomb seen in this ingest. Valid only if cn is of type 'capped'.
 *         Ignored otherwise.
 * @ptlen: length of @ptomb.
 * @ptseq: seqno of @ptomb.
 */
void
cn_tree_ingest_update(struct cn_tree *tree, struct kvset *kvset, void *ptomb, uint ptlen, u64 ptseq)
{
    struct cn_samp_stats pre, post;

    /* cn trees always have root nodes */
    assert(tree->ct_root);

    rmlock_wlock(&tree->ct_lock);
    kvset_list_add(kvset, &tree->ct_root->tn_kvset_list);
    tree->ct_root->tn_cgen++;

    cn_inc_ingest_dgen(tree->cn);

    /* Record ptomb as the max ptomb seen by this cn */
    if (cn_get_flags(tree->cn) & CN_CFLAG_CAPPED) {
        memcpy(tree->ct_last_ptomb, ptomb, ptlen);
        tree->ct_last_ptlen = ptlen;
        tree->ct_last_ptseq = ptseq;
    }

    /* update tree samp stats, get diff, and notify csched */
    cn_tree_samp(tree, &pre);
    cn_tree_samp_update_ingest(tree, tree->ct_root);
    cn_tree_samp(tree, &post);

    assert(post.i_alen >= pre.i_alen);
    assert(post.r_wlen >= pre.r_wlen);
    assert(post.l_alen == pre.l_alen);
    assert(post.l_good == pre.l_good);

    rmlock_wunlock(&tree->ct_lock);

    csched_notify_ingest(
        cn_get_sched(tree->cn), tree, post.r_alen - pre.r_alen, post.r_wlen - pre.r_wlen);
}

void
cn_tree_perfc_shape_report(
    struct cn_tree *  tree,
    struct perfc_set *rnode,
    struct perfc_set *lnode)
{
    struct cn_tree_node *tn;
    struct perfc_set *   pcv[2];
    void *               lock;

    struct {
        u64 nodec;
        u64 avglen;
        u64 maxlen;
        u64 avgsize;
        u64 maxsize;
    } ssv[2];

    memset(ssv, 0, sizeof(ssv));

    rmlock_rlock(&tree->ct_lock, &lock);
    cn_tree_foreach_node(tn, tree) {
        uint64_t len, size, i;

        i = cn_node_isroot(tn) ? 0 : 1;
        len = cn_ns_kvsets(&tn->tn_ns);
        size = cn_ns_alen(&tn->tn_ns);

        ssv[i].nodec++;
        ssv[i].avglen += len;
        ssv[i].avgsize += size;
        ssv[i].maxlen = max(ssv[i].maxlen, len);
        ssv[i].maxsize = max(ssv[i].maxsize, size);
    }
    rmlock_runlock(lock);

    pcv[0] = rnode;
    pcv[1] = lnode;

    for (size_t i = 0; i < 2; i++) {

        if (ssv[i].nodec) {
            ssv[i].avglen /= ssv[i].nodec;
            ssv[i].avgsize /= ssv[i].nodec;

            /* Report sizes in MiB */
            ssv[i].avgsize /= 1024 * 1024;
            ssv[i].maxsize /= 1024 * 1024;
        }

        perfc_set(pcv[i], PERFC_BA_CNSHAPE_NODES, ssv[i].nodec);
        perfc_set(pcv[i], PERFC_BA_CNSHAPE_AVGLEN, ssv[i].avglen);
        perfc_set(pcv[i], PERFC_BA_CNSHAPE_AVGSIZE, ssv[i].avgsize);
        perfc_set(pcv[i], PERFC_BA_CNSHAPE_MAXLEN, ssv[i].maxlen);
        perfc_set(pcv[i], PERFC_BA_CNSHAPE_MAXSIZE, ssv[i].maxsize);
    }
}

HSE_WEAK enum hse_mclass
cn_tree_node_mclass(struct cn_tree_node *tn, enum hse_mclass_policy_dtype dtype)
{
    struct mclass_policy *policy;
    enum hse_mclass_policy_age age;

    INVARIANT(tn);

    policy = cn_get_mclass_policy(tn->tn_tree->cn);
    age = cn_node_isleaf(tn) ? HSE_MPOLICY_AGE_LEAF :
        (cn_node_isroot(tn) ? HSE_MPOLICY_AGE_ROOT : HSE_MPOLICY_AGE_INTERNAL);

    return mclass_policy_get_type(policy, age, dtype);
}

uint
cn_tree_node_scatter(const struct cn_tree_node *tn)
{
    struct kvset_list_entry *le;
    uint scatter = 0;

    list_for_each_entry_reverse(le, &tn->tn_kvset_list, le_link) {
        const uint vgroups = kvset_get_vgroups(le->le_kvset);

        /* Exclude oldest kvsets with no scatter.
         */
        if (scatter + vgroups > 1)
            scatter += vgroups;
    }
    return scatter;
}

void
cn_tree_node_get_min_key(struct cn_tree_node *tn, void *kbuf, size_t kbuf_sz, uint *min_klen)
{
    struct kvset_list_entry *le;
    const void *min_key = NULL;
    void *lock;

    INVARIANT(tn && kbuf && kbuf_sz > 0 && min_klen);

    *min_klen = 0;

    rmlock_rlock(&tn->tn_tree->ct_lock, &lock);
    list_for_each_entry (le, &tn->tn_kvset_list, le_link) {
        struct kvset *kvset = le->le_kvset;
        const void *key;
        uint klen;

        kvset_get_min_key(kvset, &key, &klen);

        if (!min_key || keycmp(key, klen, min_key, *min_klen) < 0) {
            min_key = key;
            *min_klen = klen;
        }
    }
    assert(min_key && *min_klen > 0);

    memcpy(kbuf, min_key, min_t(size_t, kbuf_sz, *min_klen));
    rmlock_runlock(lock);
}

void
cn_tree_node_get_max_key(struct cn_tree_node *tn, void *kbuf, size_t kbuf_sz, uint *max_klen)
{
    struct kvset_list_entry *le;
    const void *max_key = NULL;
    void *lock;

    INVARIANT(kbuf && kbuf_sz > 0 && max_klen);

    *max_klen = 0;

    rmlock_rlock(&tn->tn_tree->ct_lock, &lock);
    list_for_each_entry (le, &tn->tn_kvset_list, le_link) {
        struct kvset *kvset = le->le_kvset;
        const void *key;
        uint klen;

        kvset_get_max_key(kvset, &key, &klen);

        if (!max_key || keycmp(key, klen, max_key, *max_klen) > 0) {
            max_key = key;
            *max_klen = klen;
        }
    }
    assert(max_key && *max_klen > 0);

    memcpy(kbuf, max_key, min_t(size_t, kbuf_sz, *max_klen));
    rmlock_runlock(lock);
}

merr_t
cn_tree_init(void)
{
    struct kmem_cache *cache;

    assert(HSE_ACP_LINESIZE >= alignof(struct cn_tree_node));

    cache = kmem_cache_create("cntreenode", cn_node_size(), HSE_ACP_LINESIZE, SLAB_PACKED, NULL);
    if (ev(!cache)) {
        return merr(ENOMEM);
    }

    cn_node_cache = cache;

    return 0;
}

void
cn_tree_fini(void)
{
    kmem_cache_destroy(cn_node_cache);
    cn_node_cache = NULL;
}

#if HSE_MOCKING
#include "cn_tree_ut_impl.i"
#include "cn_tree_compact_ut_impl.i"
#include "cn_tree_create_ut_impl.i"
#include "cn_tree_internal_ut_impl.i"
#include "cn_tree_iter_ut_impl.i"
#include "cn_tree_view_ut_impl.i"
#endif /* HSE_MOCKING */
