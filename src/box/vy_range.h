#ifndef INCLUDES_TARANTOOL_BOX_VY_RANGE_H
#define INCLUDES_TARANTOOL_BOX_VY_RANGE_H
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

#include <stdbool.h>
#include <stdint.h>

#define RB_COMPACT 1
#include <small/rb.h>
#include <small/rlist.h>

#include "iterator_type.h"
#define HEAP_FORWARD_DECLARATION
#include "salad/heap.h"
#include "trivia/util.h"
#include "vy_entry.h"
#include "vy_stat.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct index_opts;
struct key_def;
struct vy_slice;

/**
 * A pre-computed compaction plan for a range.
 *
 * The goal of compaction is to reduce read amplification.
 * All ranges for which the LSM tree has more runs per
 * level than run_count_per_level or run size larger than
 * one defined by run_size_ratio of this level are candidates
 * for compaction.
 * Unlike other LSM implementations, Vinyl can have many
 * sorted runs in a single level, and is able to compact
 * runs from any number of adjacent levels. Moreover,
 * higher levels are always taken in when compacting
 * a lower level - i.e. L1 is always included when
 * compacting L2, and both L1 and L2 are always included
 * when compacting L3.
 *
 * The lower the level is scheduled for compaction,
 * the bigger it tends to be because upper levels are
 * taken in.
 *
 * Built by vy_range_update_compaction_priority() and consumed
 * by vy_task_compaction_new().  Stores the exact set
 * of slices that should be compacted together.  The number of
 * slices in the plan (count) is also used as the compaction
 * priority: if it is 0, the range doesn't need to be compacted.
 *
 * @sa vy_range_update_compaction_priority()
 */
struct vy_compaction_plan {
	/**
	 * Array of slice pointers selected for compaction.
	 * Dynamically allocated.  NULL if no plan is set.
	 */
	struct vy_slice **slices;
	/** Number of slices in the plan. */
	int count;
	/** Allocated size of the @a slices array. */
	int capacity;
	/**
	 * Scheduling priority.  Normally equals @a count, but
	 * set to 1 for empty ranges that need coalescing.
	 */
	int priority;
	/**
	 * True if the plan includes the last (oldest) slice
	 * in the range, meaning this is a major compaction
	 * that can drop tombstones and dead tuples.
	 */
	bool is_last_level;
};

/**
 * Free the resources held by a compaction plan.
 * Use for final cleanup (range/task deletion).
 */
void
vy_compaction_plan_destroy(struct vy_compaction_plan *plan);

/**
 * Move a compaction plan from @a src to @a dst.
 * After the move, @a src is empty (allocation transferred).
 */
void
vy_compaction_plan_move(struct vy_compaction_plan *dst,
			struct vy_compaction_plan *src);

/**
 * Range of keys in an LSM tree stored on disk.
 */
struct vy_range {
	/** Unique ID of this range. */
	int64_t id;
	/**
	 * Range lower bound. NULL if range is leftmost.
	 * Both 'begin' and 'end' statements have SELECT type with
	 * the full idexed key.
	 */
	struct vy_entry begin;
	/** Range upper bound. NULL if range is rightmost. */
	struct vy_entry end;
	/** Key definition for comparing range boundaries.
	 * Contains secondary and primary key parts for secondary
	 * keys, to ensure an always distinct result for
	 * non-unique keys.
	 */
	struct key_def *cmp_def;
	/** An estimate of the number of statements in this range. */
	struct vy_disk_stmt_counter count;
	/**
	 * List of run slices in this range, linked by vy_slice->in_range.
	 * The newer a slice, the closer it to the list head.
	 */
	struct rlist slices;
	/** Number of entries in the ->slices list. */
	int slice_count;
	/** Number of statements that need to be compacted. */
	struct vy_disk_stmt_counter compaction_queue;
	/** Pre-computed compaction plan, see struct vy_compaction_plan. */
	struct vy_compaction_plan compaction_plan;
	/**
	 * If this flag is set, the range must be scheduled for
	 * major compaction, i.e. all its slices must be included
	 * in the compaction plan. The flag is set by
	 * vy_lsm_force_compaction() and cleared when the range
	 * is scheduled for compaction.
	 */
	bool needs_compaction;
	/** Number of times the range was compacted. */
	int n_compactions;
	/**
	 * Number of dumps it takes to trigger major compaction in
	 * this range, see vy_run::dump_count for more details.
	 */
	int dumps_per_compaction;
	/** Link in vy_lsm->tree. */
	rb_node(struct vy_range) tree_node;
	/** Link in vy_lsm->range_heap. */
	struct heap_node heap_node;
	/**
	 * Incremented whenever a run is added to or deleted
	 * from this range. Used invalidate read iterators.
	 */
	uint32_t version;
};

/**
 * When looking for the best range split point, this structure
 * represents a single slice begin/end - a split candidate.
 */
struct vy_split_point {
	/** Slice owning this split point. */
	struct vy_slice *slice;
	/** Key definition to compare split points. */
	struct key_def *cmp_def;
	enum vy_split_point_type {
		VY_SPLIT_POINT_BEGIN = 0,
		VY_SPLIT_POINT_END = 1,
	} type;
	/** Balance weight: total bytes of the owning slice. */
	uint64_t bytes;
};

/**
 * Return the raw key and hint for a split point boundary.
 * Used by range split selection when scoring candidates.
 */
const char *
vy_split_key(const struct vy_split_point *p, hint_t *hint);

/**
 * Compare split points for ordering in the sweep algorithm:
 * first by key, then by boundary type.
 */
int
vy_split_point_cmp(const void *a, const void *b);

/**
 * Heap of all ranges of the same LSM tree, prioritized by
 * vy_range->compaction_plan.priority.
 */
#define HEAP_NAME vy_range_heap
static inline bool
vy_range_heap_less(struct vy_range *r1, struct vy_range *r2)
{
	return r1->compaction_plan.priority > r2->compaction_plan.priority;
}
#define HEAP_LESS(h, l, r) vy_range_heap_less(l, r)
#define heap_value_t struct vy_range
#define heap_value_attr heap_node
#include "salad/heap.h"
#undef HEAP_LESS
#undef HEAP_NAME

/** Return true if a task is scheduled for a given range. */
static inline bool
vy_range_is_scheduled(struct vy_range *range)
{
	return heap_node_is_stray(&range->heap_node);
}

/**
 * Search tree of all ranges of the same LSM tree, sorted by
 * vy_range->begin. Ranges in a tree are supposed to span
 * all possible keys without overlaps.
 */
int
vy_range_tree_cmp(struct vy_range *range_a, struct vy_range *range_b);
int
vy_range_tree_key_cmp(struct vy_entry entry, struct vy_range *range);

typedef rb_tree(struct vy_range) vy_range_tree_t;
rb_gen_ext_key(MAYBE_UNUSED static inline, vy_range_tree_, vy_range_tree_t,
	       struct vy_range, tree_node, vy_range_tree_cmp,
	       struct vy_entry, vy_range_tree_key_cmp);

/**
 * Find the first range in which a given key should be looked up.
 *
 * @param tree          Range tree to search.
 * @param iterator_type Iterator type.
 * @param key           Key to look up.
 *
 * @retval              The first range to look up the key in.
 */
struct vy_range *
vy_range_tree_find_by_key(vy_range_tree_t *tree,
			  enum iterator_type iterator_type,
			  struct vy_entry key);

/**
 * Allocate and initialize a range (either a new one or for
 * restore from disk).
 *
 * @param id        Range id.
 * @param begin     Range begin (inclusive) or NULL for -inf.
 * @param end       Range end (exclusive) or NULL for +inf.
 * @param cmp_def   Key definition for comparing range boundaries.
 *
 * @retval not NULL The new range.
 * @retval NULL     Out of memory.
 */
struct vy_range *
vy_range_new(int64_t id, struct vy_entry begin, struct vy_entry end,
	     struct key_def *cmp_def);

/**
 * Free a range and all its slices.
 *
 * @param range     Range to free.
 */
void
vy_range_delete(struct vy_range *range);

/** An snprint-style function to print boundaries of a range. */
int
vy_range_snprint(char *buf, int size, const struct vy_range *range);

static inline const char *
vy_range_str(struct vy_range *range)
{
	char *buf = tt_static_buf();
	vy_range_snprint(buf, TT_STATIC_BUF_LEN, range);
	return buf;
}

/** Add a run slice to the head of a range's list. */
void
vy_range_add_slice(struct vy_range *range, struct vy_slice *slice);

/** Add a run slice to a range's list before @next_slice. */
void
vy_range_add_slice_before(struct vy_range *range, struct vy_slice *slice,
			  struct vy_slice *next_slice);

/** Remove a run slice from a range's list. */
void
vy_range_remove_slice(struct vy_range *range, struct vy_slice *slice);

/**
 * Update compaction priority of a range.
 *
 * @param range      The range.
 * @param opts       Index options.
 * @param range_size Target range size (for split detection).
 */
void
vy_range_update_compaction_priority(struct vy_range *range,
				    const struct index_opts *opts,
				    int64_t range_size);

/**
 * Update the value of range->dumps_per_compaction.
 */
void
vy_range_update_dumps_per_compaction(struct vy_range *range);

/**
 * Choose the best split key for a range.
 *
 * @param range         The range.
 * @param range_size    Target range size.
 *
 * @retval NULL         If no suitable split key found.
 * @retval not NULL     Key to split the range by.
 */
const char *
vy_range_find_best_split(struct vy_range *range, uint64_t range_size);

/**
 * Check if a range needs to be split in two.
 *
 * @param range             The range.
 * @param range_size        Target range size.
 * @param[out] p_split_key  Key to split the range by.
 *
 * @retval true             If the range needs to be split.
 */
bool
vy_range_needs_split(struct vy_range *range, int64_t range_size,
		     const char **p_split_key);

/**
 * Check if a range needs to be coalesced with adjacent
 * ranges in a range tree.
 *
 * @param range         The range.
 * @param tree          The range tree.
 * @param range_size    Target range size.
 * @param[out] p_first  The first range in the tree to coalesce.
 * @param[out] p_last   The last range in the tree to coalesce.
 *
 * @retval true         If the range needs to be coalesced.
 */
bool
vy_range_needs_coalesce(struct vy_range *range, vy_range_tree_t *tree,
			int64_t range_size, struct vy_range **p_first,
			struct vy_range **p_last);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_RANGE_H */
