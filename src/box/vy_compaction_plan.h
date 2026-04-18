#ifndef INCLUDES_TARANTOOL_BOX_VY_COMPACTION_PLAN_H
#define INCLUDES_TARANTOOL_BOX_VY_COMPACTION_PLAN_H
/*
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>

#include "vy_entry.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_slice;
struct key_def;

/**
 * One contiguous segment of a compaction's output key range.
 * The compaction walks the plan in order; each segment either
 * merges input tuples in the [begin, end) range or byte-copies
 * a run of pages from exactly one input slice.
 */
enum vy_stitch_segment_type {
	/** Produce the segment by merging all input slices. */
	VY_STITCH_MERGE,
	/** Produce the segment by byte-copying source pages. */
	VY_STITCH_COPY,
};

struct vy_stitch_segment {
	enum vy_stitch_segment_type type;
	union {
		struct {
			/**
			 * Inclusive begin of the merge range. If
			 * .stmt is NULL, the segment starts from
			 * the input's lower bound.
			 */
			struct vy_entry begin;
			/**
			 * Exclusive end of the merge range. If
			 * .stmt is NULL, the segment ends at the
			 * input's upper bound.
			 */
			struct vy_entry end;
		} merge;
		struct {
			/** Source slice providing the pages. */
			struct vy_slice *src;
			/** Inclusive first page id in src. */
			uint32_t first_page;
			/** Inclusive last page id in src. */
			uint32_t last_page;
		} copy;
	};
};

/**
 * Plan built from a set of input slices. The segs array is
 * ordered by key; consecutive segments cover adjacent key
 * ranges. The executor walks segs front to back and dispatches
 * each to either merge or copy.
 */
struct vy_stitch_plan {
	struct vy_stitch_segment *segs;
	int count;
};

/**
 * Build a stitch plan for a compaction whose inputs are
 * @a slices and whose output covers [@a range_begin,
 * @a range_end). Pages of any single input slice whose key
 * range lies entirely inside [range_begin, range_end) and does
 * not intersect any page of any other input slice are grouped
 * into COPY segments. The remaining key ranges become MERGE
 * segments. COPY runs shorter than @a min_copy_pages are
 * demoted to MERGE to amortise the per-segment overhead.
 *
 * Allocates plan->segs with xcalloc; destroy with
 * vy_stitch_plan_destroy.
 *
 * @retval 0   success; plan->count may be zero if there is no
 *             work (every slice is empty inside the range).
 * @retval -1  memory error (diag is set).
 */
int
vy_stitch_plan_build(struct vy_slice **slices, int slice_count,
		     struct vy_entry range_begin,
		     struct vy_entry range_end,
		     struct key_def *cmp_def,
		     int min_copy_pages,
		     struct vy_stitch_plan *plan);

/** Release plan's memory. */
void
vy_stitch_plan_destroy(struct vy_stitch_plan *plan);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_COMPACTION_PLAN_H */
