/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (C) 2015-2022 Micron Technology, Inc.  All rights reserved.
 */

#ifndef HSE_KVDB_CN_CSCHED_SP3_WORK_H
#define HSE_KVDB_CN_CSCHED_SP3_WORK_H

#include <error/merr.h>
#include <hse_util/inttypes.h>

/* MTF_MOCK_DECL(csched_sp3_work) */

/* clang-format off */

#define SP3_RSPILL_RUNLEN_MIN           (1u) /* root spill requires at least 1 kvset */
#define SP3_RSPILL_RUNLEN_MAX           (16u)
#define SP3_RSPILL_RUNLEN_MIN_DEFAULT   (5u)
#define SP3_RSPILL_RUNLEN_MAX_DEFAULT   (9u)

#define SP3_RSPILL_SIZEMB_MIN           (4u * 1024)
#define SP3_RSPILL_SIZEMB_MAX           (32u * 1024)
#define SP3_RSPILL_SIZEMB_MAX_DEFAULT   (8u * 1024)

#define SP3_LLEN_RUNLEN_MIN             (2u) /* length reduction requires at least 2 kvsets */
#define SP3_LLEN_RUNLEN_MAX             (16u)
#define SP3_LLEN_RUNLEN_MIN_DEFAULT     (4u)
#define SP3_LLEN_RUNLEN_MAX_DEFAULT     (8u)
#define SP3_LLEN_IDLEC_DEFAULT          (2u)  /* minimum number of kvsets */
#define SP3_LLEN_IDLEM_DEFAULT          (10u) /* minimum number of minutes */

#define SP3_LCOMP_RUNLEN_MAX            (12u)
#define SP3_LCOMP_SPLIT_PCT             (100u)
#define SP3_LCOMP_SPLIT_KEYS            (64u) /* units of 4 million */

/* clang-format on */

struct sp3_node;
struct cn_compaction_work;

/* The first work types up to but not including wtype_root are used to index
 * the work tree arrays, so be sure to add new work types before wtype_root.
 */
enum sp3_work_type {
    wtype_length = 0u,  /* leaf nodes: k-compact to reduce node length */
    wtype_garbage,      /* leaf nodes: kv-compact to reduce garbage */
    wtype_scatter,      /* leaf nodes: kv-compact to reduce vgroup scatter */
    wtype_split,        /* leaf nodes: split to eliminate large nodes */
    wtype_idle,         /* root+leaf nodes: kv-compact idle nodes */
    wtype_root,         /* root node: spill to leaves */
    wtype_MAX
};

struct sp3_thresholds {
    uint8_t  rspill_runlen_min;
    uint8_t  rspill_runlen_max;
    uint16_t rspill_sizemb_max;
    uint8_t  lcomp_runlen_max;
    uint8_t  lcomp_split_pct;     /* leaf node split-by-clen percentage threshold */
    uint8_t  lcomp_split_keys;    /* leaf node split-by-keys threshold (units of 4 million) */
    uint8_t  lscat_hwm;
    uint8_t  lscat_runlen_max;
    uint8_t  llen_runlen_min;
    uint8_t  llen_runlen_max;
    uint8_t  llen_idlec;
    uint8_t  llen_idlem;
};

/* MTF_MOCK */
merr_t
sp3_work(
    struct sp3_node            *spn,
    enum sp3_work_type          wtype,
    struct sp3_thresholds      *thresholds,
    uint                        debug,
    struct cn_compaction_work **wp);

#if HSE_MOCKING
#include "csched_sp3_work_ut.h"
#endif /* HSE_MOCKING */

#endif
