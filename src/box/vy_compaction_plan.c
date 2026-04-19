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
#include "vy_compaction_plan.h"

#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "fiber.h"
#include "small/region.h"
#include "trivia/util.h"
#include "tuple.h"

#include "salad/lcp.h"
#include "vy_run.h"
#include "vy_stmt.h"

/** Raw msgpack key slot materialized during the source page scan. */
struct vy_stitch_raw_key {
	const char *data;
	uint32_t size;
};

/** Range of consecutive source pages eligible for verbatim copy. */
struct vy_stitch_copy_run {
	uint32_t first_offset;
	uint32_t last_offset;
};

/**
 * Unref tuples owned by a plan segment. MERGE segs store refs
 * to their begin/end entries; COPY segs pin no tuples.
 */
static void
vy_stitch_segment_cleanup(struct vy_stitch_segment *seg)
{
	if (seg->type != VY_STITCH_MERGE)
		return;
	if (seg->merge.begin.stmt != NULL)
		tuple_unref(seg->merge.begin.stmt);
	if (seg->merge.end.stmt != NULL)
		tuple_unref(seg->merge.end.stmt);
}

/** Fallback: one MERGE segment covering the whole output range. */
static int
vy_stitch_plan_emit_single_merge(struct vy_stitch_plan *plan,
				 struct vy_entry range_begin,
				 struct vy_entry range_end)
{
	plan->segs = calloc(1, sizeof(*plan->segs));
	if (plan->segs == NULL) {
		diag_set(OutOfMemory, sizeof(*plan->segs),
			 "calloc", "vy_stitch_segment");
		return -1;
	}
	plan->segs[0].type = VY_STITCH_MERGE;
	plan->segs[0].merge.begin = range_begin;
	if (range_begin.stmt != NULL)
		tuple_ref(range_begin.stmt);
	plan->segs[0].merge.end = range_end;
	if (range_end.stmt != NULL)
		tuple_ref(range_end.stmt);
	plan->count = 1;
	return 0;
}

/**
 * Materialise the msgpack min_key for each page of the oldest slice
 * plus the min_key of the page immediately after the slice's last
 * page. The tail slot is NULL when the slice ends at the run's
 * last page (its upper bound is @a src ->end_bound instead).
 *
 * The returned pointers live in region-allocated memory owned by
 * the caller's region savepoint.
 */
static void
vy_stitch_extract_page_keys(struct vy_slice *src,
			    struct vy_stitch_raw_key *keys,
			    uint32_t count)
{
	struct vy_run *run = src->run;
	struct lcp_index_iter it;
	char *buf = (char *)xregion_alloc(&fiber()->gc,
					  run->lcp_index.max_key_len);
	lcp_index_iter_init(&it, &run->lcp_index, buf);
	for (uint32_t p = 0; p < src->first_page_no; p++)
		lcp_index_iter_next_key(&it);
	for (uint32_t i = 0; i < count; i++) {
		uint32_t p = src->first_page_no + i;
		if (p >= run->info.page_count) {
			keys[i].data = NULL;
			keys[i].size = 0;
			continue;
		}
		uint32_t len = lcp_index_iter_next_key(&it);
		assert(len > 0);
		char *copy = (char *)xregion_alloc(&fiber()->gc, len);
		memcpy(copy, buf, len);
		keys[i].data = copy;
		keys[i].size = len;
	}
}

/**
 * True if peer slice @a q has any key in the source page's range
 * [page_min, page_upper). When @a page_upper is NULL the upper
 * bound is taken from @a src ->end_bound instead. The check uses
 * slice-level begin/end_bound, which is a conservative upper bound
 * on where the peer's keys can physically live.
 */
static bool
vy_stitch_peer_overlaps_page(struct vy_slice *src, struct vy_slice *q,
			     const char *page_min, const char *page_upper,
			     struct key_def *cmp_def)
{
	/* q.begin >= page_upper => q starts at or after page ends. */
	if (page_upper != NULL) {
		if (vy_entry_compare_with_raw_key(q->begin, page_upper,
						  HINT_NONE, cmp_def) >= 0)
			return false;
	} else if (vy_bound_cmp(q->begin, src->end_bound, cmp_def) > 0) {
		return false;
	}
	/*
	 * q.end_bound strictly below page_min, or equal with an
	 * exclusive flag => q's keys all lie before page_min.
	 * vy_bound_cmp_raw returns -1 for the exclusive-at-equal
	 * case, matching the no-overlap answer with a single check.
	 */
	if (vy_bound_cmp_raw(q->end_bound, page_min,
			     HINT_NONE, cmp_def) < 0)
		return false;
	return true;
}

/**
 * Check that a candidate source page sits entirely inside the
 * output range. Pages that straddle a range boundary carry keys
 * that belong to neighbouring ranges and must not be copied
 * verbatim into this range's output.
 */
static bool
vy_stitch_page_fits_output_range(struct vy_slice *src,
				 const char *page_min,
				 const char *page_upper,
				 struct vy_entry range_begin,
				 struct vy_entry range_end,
				 struct key_def *cmp_def)
{
	if (range_begin.stmt != NULL &&
	    vy_entry_compare_with_raw_key(range_begin, page_min,
					  HINT_NONE, cmp_def) > 0)
		return false;
	if (range_end.stmt == NULL)
		return true;
	if (page_upper != NULL) {
		return vy_entry_compare_with_raw_key(range_end, page_upper,
						     HINT_NONE, cmp_def) >= 0;
	}
	return vy_bound_cmp(src->end_bound, range_end, cmp_def) <= 0;
}

int
vy_stitch_plan_build(struct vy_slice **slices, int slice_count,
		     struct vy_entry range_begin,
		     struct vy_entry range_end,
		     struct key_def *cmp_def,
		     int min_copy_pages,
		     struct vy_stitch_plan *plan)
{
	plan->segs = NULL;
	plan->count = 0;
	if (slice_count == 0)
		return 0;
	/*
	 * Verbatim copy needs at least one peer that gets merged;
	 * and a positive threshold that makes a copy segment worth
	 * its per-segment overhead.
	 */
	if (slice_count < 2 || min_copy_pages <= 0)
		return vy_stitch_plan_emit_single_merge(plan, range_begin,
							range_end);

	struct vy_slice *src = slices[slice_count - 1];
	struct vy_run *src_run = src->run;

	if (vy_run_is_empty(src_run))
		return vy_stitch_plan_emit_single_merge(plan, range_begin,
							range_end);
	/*
	 * The output run inherits the oldest slice's dict; the
	 * copied bytes were compressed against that dict. If any
	 * peer uses a different dict its pages cannot be appended
	 * verbatim, and the output dict choice is outside this
	 * planner's control anyway -- fall back to a plain merge.
	 */
	for (int i = 0; i < slice_count - 1; i++) {
		if (slices[i]->run->dict != src_run->dict)
			return vy_stitch_plan_emit_single_merge(
				plan, range_begin, range_end);
	}

	uint32_t n_pages = src->last_page_no - src->first_page_no + 1;
	if (n_pages < (uint32_t)min_copy_pages)
		return vy_stitch_plan_emit_single_merge(plan, range_begin,
							range_end);

	size_t region_svp = region_used(&fiber()->gc);
	struct vy_stitch_raw_key *keys = xregion_alloc(&fiber()->gc,
				(n_pages + 1) * sizeof(*keys));
	bool *eligible = xregion_alloc(&fiber()->gc,
				       n_pages * sizeof(bool));
	vy_stitch_extract_page_keys(src, keys, n_pages + 1);

	/* Mark pages eligible for verbatim copy. */
	for (uint32_t i = 0; i < n_pages; i++) {
		const char *p_min = keys[i].data;
		const char *p_upper = keys[i + 1].data;
		assert(p_min != NULL);
		eligible[i] = false;
		if (!vy_stitch_page_fits_output_range(src, p_min, p_upper,
						      range_begin, range_end,
						      cmp_def))
			continue;
		bool overlap = false;
		for (int q = 0; q < slice_count - 1; q++) {
			if (vy_stitch_peer_overlaps_page(src, slices[q],
							 p_min, p_upper,
							 cmp_def)) {
				overlap = true;
				break;
			}
		}
		eligible[i] = !overlap;
	}

	/* Coalesce eligible pages into runs, dropping short ones. */
	struct vy_stitch_copy_run *runs = xregion_alloc(&fiber()->gc,
				n_pages * sizeof(*runs));
	int run_count = 0;
	for (uint32_t i = 0; i < n_pages; ) {
		if (!eligible[i]) { i++; continue; }
		uint32_t j = i;
		while (j < n_pages && eligible[j])
			j++;
		if (j - i >= (uint32_t)min_copy_pages) {
			runs[run_count].first_offset = i;
			runs[run_count].last_offset = j - 1;
			run_count++;
		}
		i = j;
	}

	if (run_count == 0) {
		region_truncate(&fiber()->gc, region_svp);
		return vy_stitch_plan_emit_single_merge(plan, range_begin,
							range_end);
	}

	int max_segs = run_count * 2 + 1;
	struct vy_stitch_segment *segs = calloc(max_segs, sizeof(*segs));
	if (segs == NULL) {
		diag_set(OutOfMemory, max_segs * sizeof(*segs),
			 "calloc", "vy_stitch_segment");
		region_truncate(&fiber()->gc, region_svp);
		return -1;
	}
	int seg_count = 0;
	struct vy_entry cursor = range_begin;
	if (cursor.stmt != NULL)
		tuple_ref(cursor.stmt);

	for (int r = 0; r < run_count; r++) {
		uint32_t first_off = runs[r].first_offset;
		uint32_t last_off = runs[r].last_offset;
		struct vy_entry copy_start =
			vy_entry_key_from_msgpack(src_run->env->key_format,
						  cmp_def,
						  keys[first_off].data);
		if (copy_start.stmt == NULL)
			goto err_alloc;
		/*
		 * The head MERGE bridges [cursor, first-copy-key).
		 * Skip when the source's first eligible page
		 * coincides with the output's left boundary.
		 */
		bool head_empty = (cursor.stmt != NULL &&
				   vy_entry_compare(cursor, copy_start,
						    cmp_def) >= 0);
		if (!head_empty) {
			segs[seg_count].type = VY_STITCH_MERGE;
			segs[seg_count].merge.begin = cursor;
			segs[seg_count].merge.end = copy_start;
			tuple_ref(copy_start.stmt);
			seg_count++;
		} else {
			if (cursor.stmt != NULL)
				tuple_unref(cursor.stmt);
		}
		tuple_unref(copy_start.stmt);

		segs[seg_count].type = VY_STITCH_COPY;
		segs[seg_count].copy.src = src;
		segs[seg_count].copy.first_page =
			src->first_page_no + first_off;
		segs[seg_count].copy.last_page =
			src->first_page_no + last_off;
		seg_count++;

		/* Advance the cursor past the just-emitted copy. */
		if (last_off + 1 < n_pages) {
			cursor = vy_entry_key_from_msgpack(
				src_run->env->key_format, cmp_def,
				keys[last_off + 1].data);
			if (cursor.stmt == NULL)
				goto err_alloc;
		} else {
			cursor = src->end_bound;
			if (cursor.stmt != NULL)
				tuple_ref(cursor.stmt);
		}
	}

	/* Tail MERGE from cursor to range_end; may be empty. */
	bool tail_empty = false;
	if (range_end.stmt != NULL && cursor.stmt != NULL) {
		int c = vy_bound_cmp(cursor, range_end, cmp_def);
		tail_empty = (c >= 0);
	}
	if (!tail_empty) {
		segs[seg_count].type = VY_STITCH_MERGE;
		segs[seg_count].merge.begin = cursor;
		segs[seg_count].merge.end = range_end;
		if (range_end.stmt != NULL)
			tuple_ref(range_end.stmt);
		seg_count++;
	} else if (cursor.stmt != NULL) {
		tuple_unref(cursor.stmt);
	}

	plan->segs = segs;
	plan->count = seg_count;
	region_truncate(&fiber()->gc, region_svp);
	return 0;

err_alloc:
	for (int s = 0; s < seg_count; s++)
		vy_stitch_segment_cleanup(&segs[s]);
	if (cursor.stmt != NULL)
		tuple_unref(cursor.stmt);
	free(segs);
	region_truncate(&fiber()->gc, region_svp);
	return -1;
}

void
vy_stitch_plan_destroy(struct vy_stitch_plan *plan)
{
	for (int i = 0; i < plan->count; i++)
		vy_stitch_segment_cleanup(&plan->segs[i]);
	free(plan->segs);
	plan->segs = NULL;
	plan->count = 0;
}
