/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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
#include "vy_range.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RB_COMPACT 1
#include <small/rb.h>
#include <small/rlist.h>

#include "diag.h"
#include "iterator_type.h"
#include "key_def.h"
#include "trivia/util.h"
#include "tuple.h"
#include "vy_run.h"
#include "vy_stat.h"
#include "vy_stmt.h"

int
vy_range_tree_cmp(struct vy_range *range_a, struct vy_range *range_b)
{
	if (range_a == range_b)
		return 0;

	/* Any key > -inf. */
	if (range_a->begin.stmt == NULL)
		return -1;
	if (range_b->begin.stmt == NULL)
		return 1;

	assert(range_a->cmp_def == range_b->cmp_def);
	return vy_entry_compare(range_a->begin, range_b->begin,
				range_a->cmp_def);
}

int
vy_range_tree_key_cmp(struct vy_entry entry, struct vy_range *range)
{
	/* Any key > -inf. */
	if (range->begin.stmt == NULL)
		return 1;
	return vy_entry_compare(entry, range->begin, range->cmp_def);
}

struct vy_range *
vy_range_tree_find_by_key(vy_range_tree_t *tree,
			  enum iterator_type iterator_type,
			  struct vy_entry key)
{
	if (vy_stmt_is_empty_key(key.stmt)) {
		switch (iterator_type) {
		case ITER_LT:
		case ITER_LE:
		case ITER_REQ:
			return vy_range_tree_last(tree);
		case ITER_GT:
		case ITER_GE:
		case ITER_EQ:
			return vy_range_tree_first(tree);
		default:
			unreachable();
			return NULL;
		}
	}
	struct vy_range *range;
	if (iterator_type == ITER_GE || iterator_type == ITER_GT ||
	    iterator_type == ITER_EQ) {
		/**
		 * Case 1. part_count == 1, looking for [10]. ranges:
		 * {1, 3, 5} {7, 8, 9} {10, 15 20} {22, 32, 42}
		 *                      ^looking for this
		 * Case 2. part_count == 1, looking for [10]. ranges:
		 * {1, 2, 4} {5, 6, 7, 8} {50, 100, 200}
		 *            ^looking for this
		 * Case 3. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [2, 3]} {[9, 1], [10, 1], [10 2], [11 3]} {[12,..}
		 *                   ^looking for this
		 * Case 4. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [10, 1]} {[10, 2] [10 3] [11 3]} {[12, 1]..}
		 *  ^looking for this
		 * Case 5. part_count does not matter, looking for [10].
		 * ranges:
		 * {100, 200}, {300, 400}
		 * ^looking for this
		 */
		/**
		 * vy_range_tree_psearch finds least range with begin == key
		 * or previous if equal was not found
		 */
		range = vy_range_tree_psearch(tree, key);
		/* switch to previous for case (4) */
		if (range != NULL && range->begin.stmt != NULL &&
		    !vy_stmt_is_full_key(key.stmt, range->cmp_def) &&
		    vy_entry_compare(key, range->begin, range->cmp_def) == 0)
			range = vy_range_tree_prev(tree, range);
		/* for case 5 or subcase of case 4 */
		if (range == NULL)
			range = vy_range_tree_first(tree);
	} else {
		assert(iterator_type == ITER_LT || iterator_type == ITER_LE ||
		       iterator_type == ITER_REQ);
		/**
		 * Case 1. part_count == 1, looking for [10]. ranges:
		 * {1, 3, 5} {7, 8, 9} {10, 15 20} {22, 32, 42}
		 *                      ^looking for this
		 * Case 2. part_count == 1, looking for [10]. ranges:
		 * {1, 2, 4} {5, 6, 7, 8} {50, 100, 200}
		 *            ^looking for this
		 * Case 3. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [2, 3]} {[9, 1], [10, 1], [10 2], [11 3]} {[12,..}
		 *                   ^looking for this
		 * Case 4. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [10, 1]} {[10, 2] [10 3] [11 3]} {[12, 1]..}
		 *                    ^looking for this
		 * Case 5. part_count does not matter, looking for [10].
		 * ranges:
		 * {1, 2}, {3, 4, ..}
		 *          ^looking for this
		 */
		/**
		 * vy_range_tree_nsearch finds most range with begin == key
		 * or next if equal was not found
		 */
		range = vy_range_tree_nsearch(tree, key);
		if (range != NULL) {
			/* fix curr_range for cases 2 and 3 */
			if (range->begin.stmt != NULL &&
			    vy_entry_compare(key, range->begin,
					     range->cmp_def) != 0) {
				struct vy_range *prev;
				prev = vy_range_tree_prev(tree, range);
				if (prev != NULL)
					range = prev;
			}
		} else {
			/* Case 5 */
			range = vy_range_tree_last(tree);
		}
	}
	return range;
}

/**
 * Clear a compaction plan but keep the allocation for reuse.
 * Ensure the slices array can hold at least @a slice_count
 * entries plus a NULL terminator.
 */
static void
vy_compaction_plan_reset(struct vy_compaction_plan *plan, int slice_count)
{
	plan->count = 0;
	plan->priority = 0;
	plan->is_last_level = false;
	plan->split_key = NULL;
	if (plan->slices == NULL || slice_count > plan->capacity) {
		plan->slices = xrealloc(plan->slices, (slice_count + 1) *
					sizeof(*plan->slices));
		plan->capacity = slice_count;
	}
}

static inline void
vy_compaction_plan_add(struct vy_compaction_plan *plan,
		       struct vy_slice *slice)
{
	assert(plan->count < plan->capacity);
	plan->slices[plan->count++] = slice;
	plan->slices[plan->count] = NULL;
}

/**
 * Finalize the compaction plan: compute compaction_queue from
 * the selected slices. Assign priority and clear needs_compaction
 * if there is nothing to compact.
 */
static void
vy_compaction_plan_seal(struct vy_range *range)
{
	struct vy_compaction_plan *plan = &range->compaction_plan;
	assert(plan->slices != NULL);
	vy_disk_stmt_counter_reset(&range->compaction_queue);
	if (range->slice_count == 0) {
		/*
		 * Empty range: schedule for coalescing with a
		 * neighbor.  Skip the range if it spans the entire
		 * key space (begin == NULL && end == NULL), because
		 * that means it is the only range in the LSM tree
		 * and there is nobody to coalesce with.
		 */
		assert(plan->count == 0);
		if (range->begin.stmt != NULL || range->end.stmt != NULL)
			plan->priority = 1;
	} else if (plan->split_key != NULL) {
		/* Oversized range: schedule for splitting. */
		assert(plan->count == 0);
		plan->priority = range->slice_count;
	} else if (plan->count > 0) {
		for (int i = 0; i < plan->count; i++)
			vy_disk_stmt_counter_add(&range->compaction_queue,
						 &plan->slices[i]->count);
		plan->priority = plan->count;
	} else {
		/* No work for forced compaction. */
		range->needs_compaction = false;
	}
	plan->slices[plan->count] = NULL;
}

void
vy_compaction_plan_destroy(struct vy_compaction_plan *plan)
{
	free(plan->slices);
	plan->slices = NULL;
}

void
vy_compaction_plan_move(struct vy_compaction_plan *dst,
			struct vy_compaction_plan *src)
{
	assert(dst->slices == NULL);
	*dst = *src;
	src->slices = NULL;
	src->count = 0;
	src->capacity = 0;
	src->priority = 0;
	src->is_last_level = false;
	src->split_key = NULL;
}

struct vy_range *
vy_range_new(int64_t id, struct vy_entry begin, struct vy_entry end,
	     struct key_def *cmp_def)
{
	struct vy_range *range = calloc(1, sizeof(*range));
	if (range == NULL) {
		diag_set(OutOfMemory, sizeof(*range),
			 "malloc", "struct vy_range");
		return NULL;
	}
	range->id = id;
	range->begin = begin;
	if (begin.stmt != NULL)
		tuple_ref(begin.stmt);
	range->end = end;
	if (end.stmt != NULL)
		tuple_ref(end.stmt);
	range->cmp_def = cmp_def;
	rlist_create(&range->slices);
	heap_node_create(&range->heap_node);
	return range;
}

void
vy_range_delete(struct vy_range *range)
{
	if (range->begin.stmt != NULL)
		tuple_unref(range->begin.stmt);
	if (range->end.stmt != NULL)
		tuple_unref(range->end.stmt);
	vy_compaction_plan_destroy(&range->compaction_plan);

	struct vy_slice *slice, *next_slice;
	rlist_foreach_entry_safe(slice, &range->slices, in_range, next_slice)
		vy_slice_delete(slice);

	TRASH(range);
	free(range);
}

int
vy_range_snprint(char *buf, int size, const struct vy_range *range)
{
	int total = 0;
	SNPRINT(total, snprintf, buf, size, "[%" PRId64 "] (", range->id);
	if (range->begin.stmt != NULL)
		SNPRINT(total, tuple_snprint, buf, size, range->begin.stmt);
	else
		SNPRINT(total, snprintf, buf, size, "-inf");
	SNPRINT(total, snprintf, buf, size, "..");
	if (range->end.stmt != NULL)
		SNPRINT(total, tuple_snprint, buf, size, range->end.stmt);
	else
		SNPRINT(total, snprintf, buf, size, "inf");
	SNPRINT(total, snprintf, buf, size, ")");
	return total;
}

/*
 * Initialize run slice boundaries using range constraints.
 * Assumes the slice is not initialized yet.
 */
static void
vy_range_init_slice(struct vy_range *range, struct vy_slice *slice)
{
	assert(slice->count.pages == 0);
	struct vy_run *run = slice->run;
	struct vy_run_env *env = run->env;

	if (slice->begin.stmt) {
		tuple_unref(slice->begin.stmt);
		slice->begin = vy_entry_none();
	}
	if (slice->end_bound.stmt != NULL) {
		tuple_unref(slice->end_bound.stmt);
		slice->end_bound = vy_entry_none();
	}

	if (run->info.page_count == 0) {
		/* The run is empty hence the slice is empty too. */
		return;
	}

	/*
	 * The run's key range must intersect the range.
	 * A phantom slice (run data entirely outside the range)
	 * wastes disk references and confuses the compaction
	 * scheduler.
	 */
	assert(range->begin.stmt == NULL ||
	       vy_entry_compare_with_raw_key(
			range->begin, run->info.max_key,
			HINT_NONE, range->cmp_def) <= 0);
	assert(range->end.stmt == NULL ||
	       vy_entry_compare_with_raw_key(
			range->end, run->info.min_key,
			HINT_NONE, range->cmp_def) > 0);

	struct vy_page_info *page0 = vy_run_page_info(slice->run, 0);

	/* slice->begin = MAX(range::begin, run::min_key) */
	if (range->begin.stmt != NULL &&
	    vy_entry_compare_with_raw_key(range->begin, page0->min_key,
					  page0->min_key_hint,
					  range->cmp_def) >= 0) {
		slice->begin = range->begin;
		tuple_ref(range->begin.stmt);
	} else {
		/*
		 * We get here when range->begin is -inf or when
		 * range->begin < run::min_key, i.e. the run's
		 * first key is fully inside the range. We dup
		 * min_key so that the split heuristic can use
		 * slice->begin as a candidate split point.
		 */
		slice->begin =
			vy_entry_key_from_msgpack(env->key_format,
						  range->cmp_def,
						  page0->min_key);
		if (slice->begin.stmt == NULL)
			panic("failed to allocate slice begin");
	}
	/*
	 * Compute end_bound: the tightest upper bound on keys
	 * stored in this slice.
	 *
	 * When range->end <= max_key, the range boundary clips
	 * the run: the slice exposes keys strictly less than
	 * range->end (which is exclusive).
	 *
	 * When range->end > max_key (widened by a prior range
	 * split) or range->end is NULL (+inf), the run's data
	 * ends before the range boundary, so max_key is the
	 * precise inclusive upper bound.
	 */
	if (range->end.stmt != NULL &&
	    vy_entry_compare_with_raw_key(range->end, run->info.max_key,
					  HINT_NONE, range->cmp_def) <= 0) {
		slice->end_bound = range->end;
		tuple_ref(range->end.stmt);
		vy_stmt_set_flags(slice->end_bound.stmt,
				  VY_STMT_EXCLUSIVE_BOUND);
	} else {
		slice->end_bound = vy_entry_key_from_msgpack(
			env->key_format, range->cmp_def,
			run->info.max_key);
		if (slice->end_bound.stmt == NULL)
			panic("failed to allocate slice end_bound");
	}
	/** Lookup the first and the last pages spanned by the slice. */
	bool unused;
	slice->first_page_no =
		vy_page_index_find_page(run, slice->begin,
					range->cmp_def, ITER_GE,
					&unused);
	assert(slice->first_page_no < run->info.page_count);
	enum iterator_type itype =
		vy_entry_is_exclusive(slice->end_bound) ?
		ITER_LT : ITER_LE;
	slice->last_page_no =
		vy_page_index_find_page(run, slice->end_bound,
					range->cmp_def, itype,
					&unused);
	assert(slice->last_page_no < run->info.page_count);
	assert(slice->last_page_no >= slice->first_page_no);
	/** Estimate the number of statements in the slice. */
	uint32_t run_pages = run->info.page_count;
	uint32_t slice_pages = slice->last_page_no - slice->first_page_no + 1;
	slice->count.pages = slice_pages;
	slice->count.rows = DIV_ROUND_UP(run->count.rows *
					 slice_pages, run_pages);
	slice->count.bytes = DIV_ROUND_UP(run->count.bytes *
					  slice_pages, run_pages);
	slice->count.bytes_compressed = DIV_ROUND_UP(
		run->count.bytes_compressed * slice_pages, run_pages);
}

void
vy_range_add_slice(struct vy_range *range, struct vy_slice *slice)
{
	vy_range_init_slice(range, slice);
	rlist_add_entry(&range->slices, slice, in_range);
	range->slice_count++;
	vy_disk_stmt_counter_add(&range->count, &slice->count);
	range->version++;
}

void
vy_range_add_slice_before(struct vy_range *range, struct vy_slice *slice,
			  struct vy_slice *next_slice)
{
	vy_range_init_slice(range, slice);
	rlist_add_tail(&next_slice->in_range, &slice->in_range);
	range->slice_count++;
	vy_disk_stmt_counter_add(&range->count, &slice->count);
	range->version++;
}

void
vy_range_remove_slice(struct vy_range *range, struct vy_slice *slice)
{
	assert(range->slice_count > 0);
	assert(!rlist_empty(&range->slices));
	rlist_del_entry(slice, in_range);
	range->slice_count--;
	vy_disk_stmt_counter_sub(&range->count, &slice->count);
	range->version++;
}

/** A qsort element for sorting plan slices by begin key. */
struct vy_trim_point {
	/** Slice this point refers to. */
	struct vy_slice *slice;
	/** Key definition for comparing begin keys. */
	struct key_def *cmp_def;
	/** Position in plan->slices before sorting. */
	int index;
};

/** Compare trim points by slice begin key. NULL (= -inf) first. */
static int
vy_trim_point_cmp(const void *a, const void *b)
{
	const struct vy_trim_point *ea = a;
	const struct vy_trim_point *eb = b;
	if (ea->slice->begin.stmt == NULL)
		return (eb->slice->begin.stmt == NULL) ? 0 : -1;
	if (eb->slice->begin.stmt == NULL)
		return 1;
	return vy_entry_compare(ea->slice->begin, eb->slice->begin,
				ea->cmp_def);
}

/** Compare trim points by original position in the plan. */
static int
vy_trim_point_pos_cmp(const void *a, const void *b)
{
	const struct vy_trim_point *ea = a;
	const struct vy_trim_point *eb = b;
	return ea->index < eb->index ? -1 : ea->index > eb->index;
}

/**
 * Trim non-overlapping slices from the compaction plan.
 *
 * Find the largest overlap cluster among plan slices and keep
 * only those.
 *
 * A slice's data range is [slice->begin, run->info.max_key].
 * We don't use slice->end because it equals range->end for
 * all slices in the same range.
 */
static void
vy_compaction_plan_trim(struct vy_range *range,
			const struct index_opts *opts)
{
	struct vy_compaction_plan *plan = &range->compaction_plan;
	if (plan->count <= 1)
		return;
	/*
	 * Don't trim tiny ranges: trimming would leave
	 * many small run files on disk.
	 */
	if (range->count.bytes < 2 * (int64_t)opts->page_size)
		return;

	struct key_def *cmp_def = range->cmp_def;
	int slice_count = plan->count;

	/* Sort plan slices by begin key. */
	struct vy_trim_point trim[slice_count];
	struct vy_trim_point *end = trim + slice_count;
	struct vy_trim_point *point = trim;
	for (; point < end; point++) {
		*point = (struct vy_trim_point) {
			.slice = plan->slices[point - trim],
			.cmp_def = cmp_def,
			.index = point - trim,
		};
	}
	qsort(trim, slice_count, sizeof(struct vy_trim_point),
	      vy_trim_point_cmp);

	/*
	 * Sweep to find overlap clusters.  Track the maximum
	 * data end (run->info.max_key); when the next slice's
	 * begin exceeds it, a new cluster starts.  Pick the
	 * cluster with the most slices.
	 */
	struct vy_trim_point *best = trim;
	int best_len = 1;
	struct vy_trim_point *cur = trim;
	const char *max_end_key = cur->slice->run->info.max_key;

	for (point = cur + 1; point < end; point++) {
		struct vy_slice *s = point->slice;
		/* Is s disjoint from the current cluster? */
		bool disjoint = s->begin.stmt != NULL &&
			vy_entry_compare_with_raw_key(
				s->begin, max_end_key,
				HINT_NONE, cmp_def) > 0;
		if (disjoint) {
			if (point - cur > best_len) {
				best = cur;
				best_len = point - cur;
			}
			cur = point;
			max_end_key = s->run->info.max_key;
		} else {
			/* Extend the cluster. */
			if (vy_key_compare(s->run->info.max_key,
					   HINT_NONE, max_end_key,
					   HINT_NONE, cmp_def) > 0)
				max_end_key = s->run->info.max_key;
		}
	}
	/* Check the last cluster. */
	if (end - cur > best_len) {
		best = cur;
		best_len = end - cur;
	}

	if (best_len == slice_count) {
		/* All slices overlap.  Nothing to trim. */
		return;
	}
	say_verbose("compaction plan for range %s trimmed "
		    "from %d to %d slices (largest overlap cluster)",
		    vy_range_str(range), slice_count, best_len);

	/* Restore newest-first order and rewrite plan->slices. */
	qsort(best, best_len, sizeof(*best), vy_trim_point_pos_cmp);

	plan->count = best_len;
	struct vy_slice **dst = plan->slices;
	for (point = best; point < best + best_len; point++)
		*dst++ = point->slice;
	/* A single slice has nothing to merge with. */
	if (plan->count <= 1)
		plan->count = 0;
}

/**
 * To reduce write amplification caused by compaction, we follow
 * the LSM tree design. Runs in each range are divided into groups
 * called levels:
 *
 *   level 1: runs 1 .. L_1
 *   level 2: runs L_1 + 1 .. L_2
 *   ...
 *   level N: runs L_{N-1} .. L_N
 *
 * where L_N is the total number of runs, N is the total number of
 * levels, older runs have greater numbers. Runs at each subsequent
 * are run_size_ratio times larger than on the previous one. When
 * the number of runs at a level exceeds run_count_per_level, we
 * compact all its runs along with all runs from the upper levels
 * and in-memory indexes.  Including  previous levels into
 * compaction is relatively cheap, because of the level size
 * ratio.
 *
 * Given a range, this function computes the maximal level that
 * needs to be compacted and returns the number of slices in this
 * level and all preceding levels (0 if nothing to do).
 *
 * The algorithm assigns slices to levels by comparing their sizes
 * against a geometrically growing target.  Cascading compactions
 * are avoided by accounting for the estimated output run size.
 */
static int
vy_range_compaction_slice_count(struct vy_range *range,
				const struct index_opts *opts)
{
	/* Number of slices to compact (0 = nothing to do). */
	int compact_slice_count = 0;
	/* Total number of statements in checked runs. */
	struct vy_disk_stmt_counter total_stmt_count;
	vy_disk_stmt_counter_reset(&total_stmt_count);
	/* Total number of checked runs. */
	uint32_t total_run_count = 0;
	/* Estimated size of a compacted run, if compaction is scheduled. */
	uint64_t est_new_run_size = 0;
	/* The number of runs at the current level. */
	uint32_t level_run_count = 0;
	/*
	 * The target (perfect) size of a run at the current level.
	 * Calculated recurrently: the size of the next level equals
	 * the size of the previous level times run_size_ratio.
	 *
	 * For the last level we want it to be slightly greater
	 * than the size of the last (biggest, oldest) run so that
	 * all newer runs are at least run_size_ratio times smaller,
	 * because in conjunction with the fact that we never store
	 * more than one run at the last level, this will keep space
	 * amplification below 2 provided run_count_per_level is not
	 * greater than (run_size_ratio - 1).
	 *
	 * So to calculate the target size of the first level, we
	 * divide the size of the oldest run by run_size_ratio until
	 * it exceeds the size of the newest run. Note, DIV_ROUND_UP
	 * is important here, because if we used integral division,
	 * then after descending to the last level we would get a
	 * value slightly less than the last run size, not slightly
	 * greater, as we wanted to, which could increase space
	 * amplification by run_count_per_level in the worse case
	 * scenario.
	 */
	uint64_t target_run_size;

	uint64_t size;
	struct vy_slice *slice;
	slice = rlist_last_entry(&range->slices, struct vy_slice, in_range);
	size = MAX(slice->count.bytes, 1);
	slice = rlist_first_entry(&range->slices, struct vy_slice, in_range);
	do {
		target_run_size = size;
		size = DIV_ROUND_UP(target_run_size, opts->run_size_ratio);
	} while (size > (uint64_t)MAX(slice->count.bytes, 1));

	rlist_foreach_entry(slice, &range->slices, in_range) {
		size = slice->count.bytes;
		level_run_count++;
		total_run_count++;
		vy_disk_stmt_counter_add(&total_stmt_count, &slice->count);
		while (size > target_run_size) {
			/*
			 * The run size exceeds the threshold
			 * set for the current level. Move this
			 * run down to a lower level. Switch the
			 * current level and reset the level run
			 * count.
			 */
			level_run_count = 1;
			/*
			 * If we have already scheduled
			 * a compaction of an upper level, and
			 * estimated compacted run will end up at
			 * this level, include the new run into
			 * this level right away to avoid
			 * a cascading compaction.
			 */
			if (est_new_run_size > target_run_size)
				level_run_count++;
			/*
			 * Calculate the target run size for this
			 * level.
			 */
			target_run_size *= opts->run_size_ratio;
			/*
			 * Keep pushing the run down until
			 * we find an appropriate level for it.
			 */
		}
		/*
		 * Since all ranges constituting an LSM tree have
		 * the same configuration, they tend to get compacted
		 * simultaneously, leading to IO load spikes and, as
		 * a result, distortion of the LSM tree shape and
		 * increased read amplification. To prevent this from
		 * happening, we constantly randomize compaction pace
		 * among ranges by deferring compaction at each LSM
		 * tree level with some fixed small probability.
		 *
		 * Note, we can't use rand() directly here, because
		 * this function is called on every memory dump and
		 * scans all LSM tree levels. Instead we use the
		 * value of rand() from the slice creation time.
		 */
		uint32_t max_run_count = opts->run_count_per_level;
		if (slice->seed < RAND_MAX / 10) {
			max_run_count++;
			say_verbose("Randomizing compaction of slice %" PRId64
				    " with seed %u", slice->id, slice->seed);
		}
		if (level_run_count > max_run_count) {
			/*
			 * The number of runs at the current level
			 * exceeds the configured maximum. Arrange
			 * for compaction. We compact all runs at
			 * this level and upper levels.
			 */
			compact_slice_count = total_run_count;
			est_new_run_size = total_stmt_count.bytes;
		}
	}

	if (level_run_count > 1) {
		/*
		 * Do not store more than one run at the last level
		 * to keep space amplification low.
		 */
		compact_slice_count = total_run_count;
	}

	return compact_slice_count;
}

/**
 * Check if the range needs shape-based compaction and populate
 * the compaction plan with the topmost slices that need it.
 */
static void
vy_compaction_plan_check_shape(struct vy_range *range,
			       const struct index_opts *opts)
{
	assert(range->slice_count >= 2);
	int count = 0;
	if (range->needs_compaction) {
		count = range->slice_count;
	} else {
		count = vy_range_compaction_slice_count(range, opts);
	}
	if (count == 0)
		return;
	struct vy_slice *slice;
	rlist_foreach_entry(slice, &range->slices, in_range) {
		vy_compaction_plan_add(&range->compaction_plan, slice);
		if (range->compaction_plan.count == count)
			break;
	}
	/*
	 * Compute is_last_level before calling trim.  The flag is
	 * true when no slice outside the plan has an older version
	 * of any key inside it.  When all slices are initially
	 * selected (count == slice_count) the oldest slice is in the
	 * plan and there are no versions below it.  Trim can only
	 * remove slices from the plan, never add new ones, so if it
	 * removes the oldest slice the remaining cluster still has
	 * no older versions outside: the flag correctly survives the
	 * trim.
	 */
	range->compaction_plan.is_last_level =
		range->compaction_plan.count == range->slice_count;
	vy_compaction_plan_trim(range, opts);
}

void
vy_range_update_compaction_priority(struct vy_range *range,
				    const struct index_opts *opts,
				    int64_t range_size)
{
	assert(opts->run_count_per_level > 0);
	assert(opts->run_size_ratio > 1);

	vy_range_update_dumps_per_compaction(range);

	vy_compaction_plan_reset(&range->compaction_plan, range->slice_count);
	vy_disk_stmt_counter_reset(&range->compaction_queue);
	if (range->slice_count >= 1) {
		/*
		 * Check if the range needs splitting before developing
		 * the compaction plan.  Split is cheap (runs in the
		 * scheduler fiber) and takes priority over compaction:
		 * after the split, children inherit needs_compaction
		 * and get their own compaction plans.
		 */
		const char *split_key;
		if (vy_range_needs_split(range, range_size, &split_key)) {
			range->compaction_plan.split_key = split_key;
		} else if (range->slice_count >= 2) {
			vy_compaction_plan_check_shape(range, opts);
		}
	}
	vy_compaction_plan_seal(range);
}

void
vy_range_update_dumps_per_compaction(struct vy_range *range)
{
	if (!rlist_empty(&range->slices)) {
		struct vy_slice *slice = rlist_last_entry(&range->slices,
						struct vy_slice, in_range);
		range->dumps_per_compaction = slice->run->dump_count;
	} else {
		range->dumps_per_compaction = 0;
	}
}

const char *
vy_split_key(const struct vy_split_point *p, hint_t *hint)
{
	struct vy_slice *slice = p->slice;
	if (p->type == VY_SPLIT_POINT_BEGIN) {
		if (slice->begin.stmt != NULL) {
			*hint = slice->begin.hint;
			return tuple_data(slice->begin.stmt);
		}
		*hint = HINT_NONE;
		return slice->run->info.min_key;
	}
	/* END: use end_bound. */
	*hint = slice->end_bound.hint;
	return tuple_data(slice->end_bound.stmt);
}

/**
 * Compare two split points by key, then by boundary type.
 */
int
vy_split_point_cmp(const void *a, const void *b)
{
	const struct vy_split_point *pa = a;
	const struct vy_split_point *pb = b;

	hint_t hint_a, hint_b;
	const char *key_a = vy_split_key(pa, &hint_a);
	const char *key_b = vy_split_key(pb, &hint_b);
	int rc = vy_key_compare(key_a, hint_a, key_b, hint_b,
				pa->cmp_def);
	if (rc != 0)
		return rc;
	/* BEGIN < END at the same key. */
	if (pa->type != pb->type)
		return (int)pa->type - (int)pb->type;
	/*
	 * Among ENDs at the same key: exclusive before inclusive.
	 * An inclusive end's weight transfer must be deferred
	 * until we advance past this key, so it must sort after
	 * any exclusive end that is evaluated as a candidate here.
	 */
	if (pa->type == VY_SPLIT_POINT_END)
		return (int)!vy_entry_is_exclusive(pa->slice->end_bound) -
		       (int)!vy_entry_is_exclusive(pb->slice->end_bound);
	return 0;
}

/** Update left/right/active balance counters for split points. */
static void
vy_split_update_balance(struct vy_split_point *start,
			struct vy_split_point *end,
			uint64_t *left, uint64_t *right, uint64_t *active)
{
	for (struct vy_split_point *p = start; p < end; ++p) {
		switch (p->type) {
		case VY_SPLIT_POINT_BEGIN:
			/* When advancing to a point which is
			 * right to the begin we must add
			 * slice weight to 'active' and subtract it from
			 * 'right'.
			 */
			*active += p->bytes;
			*right -= p->bytes;
			break;
		case VY_SPLIT_POINT_END:
			if (!vy_entry_is_exclusive(p->slice->end_bound)) {
				/*
				 * Inclusive end: the slice is fully
				 * to the left only past this key.
				 */
				*left += p->bytes;
				*active -= p->bytes;
			}
			/*
			 * Exclusive end: the weight was already
			 * transferred eagerly when this point
			 * became a split candidate.
			 */
			break;
		}
	}
}

/**
 * Sweep-based split key search minimizing slice splits.
 */
const char *
vy_range_find_best_split(struct vy_range *range, uint64_t range_size)
{
	uint64_t total = 0, left = 0, right = 0;
	const char *best_key = NULL;

	/*
	 * Each slice contributes a BEGIN point and an END point.
	 * The END type (INCLUSIVE or EXCLUSIVE) is determined by
	 * slice->end_bound, which already picks the tightest
	 * upper bound (run->info.max_key or range boundary).
	 */
	size_t point_vec_size = range->slice_count * 2;

	if (range->slice_count == 0)
		return NULL;

	struct vy_split_point *point_vec =
		xmalloc(sizeof(*point_vec) * point_vec_size);

	struct vy_split_point *p = point_vec;
	struct vy_slice *slice;

	rlist_foreach_entry(slice, &range->slices, in_range) {
		*p++ = (struct vy_split_point) {
			slice, range->cmp_def,
			VY_SPLIT_POINT_BEGIN,
			slice->count.bytes
		};
		*p++ = (struct vy_split_point) {
			slice, range->cmp_def,
			VY_SPLIT_POINT_END,
			slice->count.bytes
		};
		/**
		 * We could trust range->count.bytes, but best
		 * if the algorithm does not rely on
		 * range->count.bytes == sum(slice->count.bytes).
		 */
		total += slice->count.bytes;
	}
	point_vec_size = p - point_vec;

	qsort(point_vec, point_vec_size, sizeof(*point_vec),
	      vy_split_point_cmp);

	right = total;
	/* The total size of runs split by the current split key. */
	uint64_t active = 0;
	/* The lowest byte count of runs being cut by the best split key. */
	uint64_t best_active = UINT64_MAX;
	/* The lowest size difference of left and right ranges. */
	uint64_t best_balance = UINT64_MAX;
	/*
	 * If the previous point was a begin,
	 * we increase 'active' *after* we advance to the next
	 * point.
	 * If it was an end, it's not a good split candidate, so
	 * we skip it. But we can move the slice fully to the
	 * left only when we move to a split point that's fully
	 * to the right of an inclusive end.
	 */
	struct vy_split_point *p_prev = NULL;
	for (p = point_vec; p < point_vec + point_vec_size; p++) {
		if (p->type == VY_SPLIT_POINT_END &&
		    !vy_entry_is_exclusive(p->slice->end_bound)) {
			/*
			 * Inclusive end is never a split
			 * candidate, because it still cuts off
			 * a little piece of a slice into a new
			 * range.
			 */
			continue;
		}
		if (p->type == VY_SPLIT_POINT_BEGIN && p_prev != NULL) {
			hint_t hint_p, hint_prev;
			const char *key_p = vy_split_key(p, &hint_p);
			const char *key_prev = vy_split_key(p_prev,
							    &hint_prev);
			if (vy_key_compare(key_p, hint_p,
					   key_prev, hint_prev,
					   p->cmp_def) == 0) {
				/*
				 * Multiple equal BEGINs: they "move"
				 * together from left to right, so we
				 * shift them all at once when advancing
				 * to the next split candidate.
				 */
				continue;
			}
		}
		vy_split_update_balance(p_prev ? p_prev : point_vec,
					p, &left, &right, &active);
		if (p->type == VY_SPLIT_POINT_END) {
			/* Exclusive end: eagerly transfer weight. */
			left += p->bytes;
			active -= p->bytes;
		}
		/*
		 * Splitting a run that does not participate in
		 * future compaction may permanently increase
		 * space amplification and page index size.
		 *
		 * We cannot reliably predict whether a run will
		 * be compacted in the future. The only observable
		 * proxy for the risk of space amplification is
		 * the number (or total size) of slices that
		 * overlap the split point ("active" slices).
		 *
		 * Therefore, the split key selection follows
		 * a two-step heuristic:
		 *
		 * 1. The resulting left/right ranges must be
		 *    reasonably balanced in size.
		 *
		 * 2.  Among all candidate keys that satisfy the
		 *     above constraint, we choose the one that
		 *     minimizes the amount of overlapping runs.
		 */
		uint64_t small, large;
		if (left < right) {
			small = left;
			large = right;
		} else {
			small = right;
			large = left;
		}
		uint64_t balance = large - small;

		if ((small > range_size * 2 / 3 || small > total / 3) &&
		    (best_key == NULL || active < best_active ||
		     (active == 0 && balance < best_balance))) {
			hint_t _u;
			best_active = active;
			best_balance = balance;
			best_key = vy_split_key(p, &_u);
		}
		p_prev = p;
	}
	/* Call last time to satisfy the assert below. */
	vy_split_update_balance(p_prev ? p_prev : point_vec,
				p, &left, &right, &active);
	assert(active == 0);

	free(point_vec);

	return best_key;
}

/**
 * Return true and set split_key accordingly if the range needs to be
 * split in two.
 *
 * The decision is two-phase:
 *
 * 1. First, try the sweep-based balance heuristic
 *    (vy_range_find_best_split): it picks a split key that minimizes
 *    the number of runs shared (cut) between the two new ranges while
 *    keeping both halves reasonably balanced. This is attempted once
 *    the range grows to at least 3/2 * range_size -- the 3/2 threshold
 *    ensures both halves can satisfy the inner balance requirement
 *    (each >= 1/3 of total, i.e. >= range_size/2).
 *
 * 2. If no good sweep-based split is found and the range has grown to
 *    at least 2 * range_size, fall back to splitting around the median
 *    key of the last (oldest) level's page index. This fallback
 *    requires at least one prior compaction (n_compactions >= 1) so
 *    the last level provides a reasonable approximation of the key
 *    distribution.
 */
bool
vy_range_needs_split(struct vy_range *range, int64_t range_size,
		     const char **p_split_key)
{
	if (range->count.bytes < range_size * 3 / 2) {
		/*
		 * Allow the range to grow at least 50% beyond
		 * range_size to avoid oscillation of split and
		 * coalesce. We coalesce two ranges only if each
		 * shrinks below 50% of range_size.
		 *
		 * This threshold also ensures the sweep heuristic
		 * can find a balanced split: with total >= 1.5 *
		 * range_size, both halves can be >= range_size / 2,
		 * satisfying the inner balance requirement
		 * (each half > total / 3).
		 */
		return false;
	}
	/*
	 * Try to find a split key which would:
	 * - minimize the binary footprint of runs that are
	 *   *shared* between the two formed ranges, e.g. are cut
	 *   in the middle
	 * - preserve the needed balance of left and right range
	 *   sizes.
	 */
	*p_split_key = vy_range_find_best_split(range, range_size);
	if (*p_split_key)
		return true;
	/*
	 * We haven't found a nice way to cut the range into two while
	 * keeping the amount of runs that are shared between the
	 * newly formed ranges low. Proceed further only if the
	 * range is really big.
	 */
	if (range->count.bytes < range_size * 2)
		return false;
	struct vy_slice *slice;
	/*
	 * The next step is to find a median key by looking up in
	 * the page index of the last level, assuming a uniform
	 * distribution of keys. Then, if the range has been
	 * compacted at least once, the last level should provide
	 * a good approximation of key distribution in the range.
	 */
	if (range->n_compactions < 1)
		return false;

	/* Find the oldest run. */
	assert(!rlist_empty(&range->slices));
	slice = rlist_last_entry(&range->slices, struct vy_slice, in_range);

	/* The range is too small to be split. */
	if (slice->count.bytes < range_size * 4 / 3)
		return false;

	/* Find the median key in the oldest run (approximately). */
	struct vy_page_info *mid_page;
	mid_page = vy_run_page_info(slice->run, slice->first_page_no +
				    (slice->last_page_no -
				     slice->first_page_no) / 2);

	struct vy_page_info *first_page = vy_run_page_info(slice->run,
						slice->first_page_no);

	/* No point in splitting if a new range is going to be empty. */
	if (vy_key_compare(first_page->min_key, first_page->min_key_hint,
			   mid_page->min_key, mid_page->min_key_hint,
			   range->cmp_def) == 0)
		return false;
	/*
	 * In extreme cases the median key can be < the beginning
	 * of the slice, e.g.
	 *
	 * RUN:
	 * ... |---- page N ----|-- page N + 1 --|-- page N + 2 --
	 *     | min_key = [10] | min_key = [50] | min_key = [100]
	 *
	 * SLICE:
	 * begin = [30], end = [70]
	 * first_page_no = N, last_page_no = N + 1
	 *
	 * which makes mid_page_no = N and mid_page->min_key = [10].
	 *
	 * In such cases there's no point in splitting the range.
	 */
	if (slice->begin.stmt != NULL &&
	    vy_entry_compare_with_raw_key(slice->begin, mid_page->min_key,
					  mid_page->min_key_hint,
					  range->cmp_def) >= 0)
		return false;
	/*
	 * The median key can't be >= the end of the slice as we
	 * take the min key of a page for the median key.
	 */
	assert(vy_entry_compare_with_raw_key(slice->end_bound,
					     mid_page->min_key,
					     mid_page->min_key_hint,
					     range->cmp_def) > 0);
	*p_split_key = mid_page->min_key;

	say_verbose("range %" PRId64 " exceeds %" PRIu64 " and needs split: "
		    "has %d slices, %" PRIu64 " bytes, last slice has "
		    "%" PRIu64 " pages and %" PRIu64 " bytes",
		    range->id, range_size, range->slice_count,
		    range->count.bytes, slice->count.pages,
		    slice->count.bytes);
	return true;
}

/**
 * Check if a range should be coalesced with one or more its neighbors.
 * If it should, return true and set @p_first and @p_last to the first
 * and last ranges to coalesce, otherwise return false.
 *
 * We coalesce ranges together when they become too small, less than
 * half the target range size to avoid split-coalesce oscillations.
 */
bool
vy_range_needs_coalesce(struct vy_range *range, vy_range_tree_t *tree,
			int64_t range_size, struct vy_range **p_first,
			struct vy_range **p_last)
{
	struct vy_range *it;

	/* Size of the coalesced range. */
	uint64_t total_size = range->count.bytes;
	/* Coalesce ranges until total_size > max_size. */
	uint64_t max_size = range_size / 2;

	/*
	 * We can't coalesce a range that was scheduled for dump
	 * or compaction, because it is about to be processed by
	 * a worker thread.
	 */
	assert(!vy_range_is_scheduled(range));

	/*
	 * An empty range (no slices) should be unconditionally
	 * coalesced with any unscheduled neighbor, regardless
	 * of size.  Try right neighbor first, then left.
	 */
	if (range->slice_count == 0) {
		*p_first = *p_last = range;
		it = vy_range_tree_next(tree, range);
		if (it != NULL && !vy_range_is_scheduled(it)) {
			*p_last = it;
			return true;
		}
		it = vy_range_tree_prev(tree, range);
		if (it != NULL && !vy_range_is_scheduled(it)) {
			*p_first = it;
			return true;
		}
		return false;
	}

	*p_first = *p_last = range;
	for (it = vy_range_tree_next(tree, range);
	     it != NULL && !vy_range_is_scheduled(it);
	     it = vy_range_tree_next(tree, it)) {
		uint64_t size = it->count.bytes;
		if (total_size + size > max_size)
			break;
		total_size += size;
		*p_last = it;
	}
	for (it = vy_range_tree_prev(tree, range);
	     it != NULL && !vy_range_is_scheduled(it);
	     it = vy_range_tree_prev(tree, it)) {
		uint64_t size = it->count.bytes;
		if (total_size + size > max_size)
			break;
		total_size += size;
		*p_first = it;
	}
	return *p_first != *p_last;
}
