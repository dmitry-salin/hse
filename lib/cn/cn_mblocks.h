/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_KVS_CN_MBLOCKS_H
#define HSE_KVS_CN_MBLOCKS_H

/* MTF_MOCK_DECL(cn_mblocks) */

struct mpool;
struct cndb;
struct kvset_mblocks;

/**
 * enum cn_mutation
 * @CN_MUT_OTHER:
 * @CN_MUT_KCOMPACT: key compaction
 * @CN_MUT_INGEST: CN ingest
 */
enum cn_mutation {
    CN_MUT_OTHER,
    CN_MUT_KCOMPACT,
    CN_MUT_INGEST,
};

/* flags for cn_mb_est_alen() */
#define CN_MB_EST_FLAGS_NONE (0)
#define CN_MB_EST_FLAGS_PREALLOC (1u << 0) /* preallocate w/ max_captgt */
#define CN_MB_EST_FLAGS_TRUNCATE (1u << 1) /* truncation enabled */
#define CN_MB_EST_FLAGS_POW2 (1u << 2)     /* round mblk sz to power of 2 */

/* MTF_MOCK */
size_t
cn_mb_est_alen(size_t max_captgt, size_t alloc_unit, size_t payload, uint flags);

/**
 * cn_mblocks_commit()
 * @ds:
 * @cndb:
 * @cnid:
 * @txid:
 * @num_lists:
 * @list:
 * @mutation:
 * @vcommitted:
 *      Ignored if the mutation is CN_MUT_KCOMPACT
 *      Else, number of vblocks already committed.
 *      If NULL, none of the vblocks are already committed.
 * @n_committed:
 * @context:
 * @tags:
 */
/* MTF_MOCK */
merr_t
cn_mblocks_commit(
    struct mpool *        ds,
    struct cndb *         cndb,
    u64                   cnid,
    u64                   txid,
    u32                   num_lists,
    struct kvset_mblocks *list,
    enum cn_mutation      mutation,
    u32 *                 n_committed,
    u64 *                 context,
    u64 *                 tags);

/* MTF_MOCK */
void
cn_mblocks_destroy(
    struct mpool *        ds,
    u32                   num_lists,
    struct kvset_mblocks *list,
    bool                  kcompact,
    u32                   n_committed);

#if HSE_MOCKING
#include "cn_mblocks_ut.h"
#endif /* HSE_MOCKING */

#endif
