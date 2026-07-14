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
#include "vy_mem.h"

#include <stdlib.h>

#include <trivia/util.h>
#include <small/slab_arena.h>
#include <small/slab_cache.h>
#include <small/mempool.h>
#include <small/quota.h>

#include "diag.h"
#include "fiber_cond.h"
#include "tuple.h"
#include "vy_history.h"
#include "vy_quota.h"

/** {{{ vy_mem_env */

enum {
	/** Slab size for vinyl's tuple and tree-extent arena. */
	SLAB_SIZE = 4 * 1024 * 1024
};

/** Allocate a BPS tree extent from the mem env's tree-extent pool. */
static void *
vy_mem_tree_extent_alloc(struct matras_allocator *allocator)
{
	struct vy_mem_env *env = container_of(allocator, struct vy_mem_env,
					      matras_allocator);
	void *ret = mempool_alloc(&env->tree_extent_pool);
	if (ret == NULL)
		diag_set(OutOfMemory, VY_MEM_TREE_EXTENT_SIZE,
			 "mempool_alloc", "tree_extent_pool");
	/*
	 * BPS tree extents are intentionally not charged to vy_quota.
	 * A draining mem frees its extents only when it is finally
	 * torn down, so counting them would let the not-yet-freed
	 * backlog pin the quota above its limit and spin the dump
	 * scheduler. They stay bounded indirectly: extent count
	 * tracks tuple count, and tuples are quota-throttled.
	 */
	return ret;
}

static void
vy_mem_tree_extent_free(struct matras_allocator *allocator, void *p)
{
	struct vy_mem_env *env = container_of(allocator, struct vy_mem_env,
					      matras_allocator);
	mempool_free(&env->tree_extent_pool, p);
}

/** Initialize a lazy-drain queue: empty FIFO, iterator parked invalid. */
static inline void
vy_mem_drain_create(struct vy_mem_drain *drain)
{
	rlist_create(&drain->queue);
	drain->pos = vy_mem_tree_invalid_iterator();
}

void
vy_mem_drain_step(struct vy_mem_drain *drain, size_t bytes)
{
	size_t drained = 0;
	while (bytes > drained && !rlist_empty(&drain->queue)) {
		struct vy_mem *mem =
			rlist_first_entry(&drain->queue,
					  struct vy_mem, in_mems);
		assert(mem->state == VY_MEM_DRAINING);
		/*
		 * Lazy open: the iterator is invalid both when the
		 * queue head has just rotated and when the previous
		 * head was exhausted -- the same predicate covers
		 * both transitions.
		 */
		if (vy_mem_tree_iterator_is_invalid(&drain->pos))
			drain->pos = vy_mem_tree_first(&mem->tree);
		while (bytes > drained &&
		       !vy_mem_tree_iterator_is_invalid(&drain->pos)) {
			struct vy_entry *elem =
				vy_mem_tree_iterator_get_elem(&mem->tree,
							      &drain->pos);
			/*
			 * get_elem() returns NULL at the end; is_invalid()
			 * flips to true automatically then.
			 */
			if (elem == NULL)
				break;
			size_t size = tuple_size(elem->stmt);
			vy_stmt_unref(elem->stmt);
			vy_mem_tree_iterator_next(&mem->tree, &drain->pos);
			drained += size;
		}
		if (vy_mem_tree_iterator_is_invalid(&drain->pos)) {
			rlist_del_entry(mem, in_mems);
			vy_mem_destroy(mem);
		}
	}
}

void
vy_mem_env_create(struct vy_mem_env *env, size_t memory,
		  struct vy_quota_acct *quota_acct, struct vy_tx_manager *xm)
{
	/*
	 * The arena holds vinyl tuples and the in-memory tree extents.
	 * vinyl_memory is only its prealloc: the arena quota is left
	 * unbounded because vy_quota does the limiting logically, and
	 * that limit is soft -- it may overshoot vinyl_memory a little,
	 * so the arena must stay free to grow past the prealloc.
	 */
	quota_init(&env->quota, QUOTA_MAX);
	tuple_arena_create(&env->arena, &env->quota, memory,
			   SLAB_SIZE, false, "vinyl");
	slab_cache_create(&env->cache, &env->arena);
	mempool_create(&env->tree_extent_pool, &env->cache,
		       VY_MEM_TREE_EXTENT_SIZE);
	matras_allocator_create(&env->matras_allocator,
				VY_MEM_TREE_EXTENT_SIZE,
				vy_mem_tree_extent_alloc,
				vy_mem_tree_extent_free);
	env->quota_acct = quota_acct;
	env->xm = xm;
	vy_mem_drain_create(&env->drain);
}

void
vy_mem_env_destroy(struct vy_mem_env *env)
{
	/*
	 * Called only at process shutdown, after which the OS reclaims
	 * everything. Spaces are not dropped on shutdown (schema_free()
	 * is disabled), so live mems never reach vy_mem_delete; their
	 * tuples (and the tuple cache's) stay allocated in the arena.
	 * Leak the arena rather than destroy it -- like memtx, which
	 * never destroys its tuple arena -- because tuple_arena_destroy
	 * would assert that every slab has been returned.
	 */
	matras_allocator_destroy(&env->matras_allocator);
	mempool_destroy(&env->tree_extent_pool);
	slab_cache_destroy(&env->cache);
}

/* }}} vy_mem_env */

/** {{{ vy_mem */

struct vy_mem *
vy_mem_new(struct vy_mem_env *env, struct vy_mem_stat *lsm_stat,
	   struct key_def *cmp_def, struct tuple_format *format,
	   int64_t generation, uint32_t space_cache_version, uint32_t iid)
{
	size_t size = sizeof(struct vy_mem);
	if (iid == 0)
		size += sizeof(struct vy_stmt_stat);
	struct vy_mem *index = xcalloc(1, size);
	if (iid == 0)
		index->stmt = (struct vy_stmt_stat *)(index + 1);
	index->env = env;
	index->lsm_stat = lsm_stat;
	index->dump_lsn = -1;
	index->cmp_def = cmp_def;
	index->generation = generation;
	index->space_cache_version = space_cache_version;
	index->format = format;
	tuple_format_ref(format);
	vy_mem_tree_create(&index->tree, cmp_def,
			   &env->matras_allocator);
	index->state = VY_MEM_ACTIVE;
	rlist_create(&index->in_mems);
	fiber_cond_create(&index->pin_cond);
	return index;
}

void
vy_mem_delete(struct vy_mem *index)
{
	assert(index->state != VY_MEM_DRAINING);
	if (index->state == VY_MEM_SEALED)
		rlist_del_entry(index, in_mems);
	index->state = VY_MEM_DRAINING;
	rlist_add_tail_entry(&index->env->drain.queue, index, in_mems);
	/*
	 * Bytes are freed from the write-path perspective the moment
	 * the mem is detached; the slab slots stay occupied until
	 * vy_mem_drain_step visits each tuple.
	 */
	assert(index->stat.count.bytes >= 0);
	vy_mem_stat_sub(index->lsm_stat, &index->stat);
	vy_mem_stat_sub(&index->env->stat, &index->stat);
	/*
	 * Return only what this mem charged (its uniq_bytes). Statements
	 * shared with the primary were charged there, so a secondary's
	 * uniq_bytes covers just its sole-owned statements; the primary's
	 * covers everything it holds. The sum stays in step with the quota.
	 */
	vy_quota_unacct(index->env->quota_acct, index->stat.uniq_bytes);
}

void
vy_mem_destroy(struct vy_mem *index)
{
	vy_mem_tree_destroy(&index->tree);
	tuple_format_unref(index->format);
	fiber_cond_destroy(&index->pin_cond);
	TRASH(index);
	free(index);
}

struct vy_entry
vy_mem_older_lsn(struct vy_mem *mem, struct vy_entry entry)
{
	struct vy_mem_tree_key tree_key;
	tree_key.entry = entry;
	tree_key.lsn = vy_stmt_lsn(entry.stmt) - 1;
	bool exact = false;
	struct vy_mem_tree_iterator itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, &exact);

	if (vy_mem_tree_iterator_is_invalid(&itr))
		return vy_entry_none();

	struct vy_entry result;
	result = *vy_mem_tree_iterator_get_elem(&mem->tree, &itr);
	if (vy_entry_compare(result, entry, mem->cmp_def) != 0)
		return vy_entry_none();
	return result;
}

/**
 * Find the older version of this key at the given tree position.
 * The tree is ordered by (key, lsn DESC); advancing one step from
 * @a at yields the older same-key entry if one exists.
 */
static inline struct vy_entry
vy_mem_older_at(struct vy_mem *mem, struct vy_mem_tree_iterator *at)
{
	struct vy_mem_tree_iterator next = *at;
	vy_mem_tree_iterator_next(&mem->tree, &next);
	if (vy_mem_tree_iterator_is_invalid(&next))
		return vy_entry_none();
	struct vy_entry older =
		*vy_mem_tree_iterator_get_elem(&mem->tree, &next);
	struct vy_entry cur =
		*vy_mem_tree_iterator_get_elem(&mem->tree, at);
	if (vy_entry_compare(cur, older, mem->cmp_def) != 0)
		return vy_entry_none();
	return older;
}

/**
 * Predicate: does this statement contribute to per-type stats
 * used by space:len()?
 *
 * INSERT is always counted: vinyl enforces uniqueness, so a
 * committed INSERT represents a genuinely new key.
 *
 * DELETE is counted unless it tombstones what is already a
 * tombstone (blind re-delete); only the non-DELETE older case
 * actually removes a live key.
 *
 * REPLACE and UPSERT are counted only when they recreate a
 * key -- older is absent or itself a tombstone. Otherwise they
 * rewrite a live key and belong to the blind-write correction,
 * not the baseline.
 */
static inline bool
vy_mem_stmt_is_counted(enum iproto_type type, struct vy_entry older)
{
	if (type == IPROTO_INSERT)
		return true;
	if (older.stmt == NULL)
		return true;
	enum iproto_type older_type = vy_stmt_type(older.stmt);
	if (type == IPROTO_DELETE)
		return older_type != IPROTO_DELETE;
	return older_type == IPROTO_DELETE;
}

/**
 * Single point that updates the in-memory counters (per-mem, per-LSM,
 * engine-wide) and the quota for one @a stmt entering @a mem, so
 * callers can't drift them apart. vy_mem_delete releases a mem's whole
 * contribution in bulk from its own stat.
 */
static inline void
vy_mem_acct(struct vy_mem *mem, struct tuple *stmt,
	    enum vy_quota_consumer_type consumer)
{
	vy_stmt_counter_acct_tuple(&mem->stat.count, stmt);
	vy_stmt_counter_acct_tuple(&mem->lsm_stat->count, stmt);
	vy_stmt_counter_acct_tuple(&mem->env->stat.count, stmt);
	/*
	 * Charge the quota once per distinct physical statement. VY_STMT_MEM
	 * marks a statement already resident in some mem: a tuple shared
	 * across the primary and secondary mems is charged by the first mem
	 * it enters (the primary, inserted first on commit); the later
	 * inserts of the same pointer find the flag set and skip the charge.
	 * A sole-owned secondary statement -- a deferred-delete tombstone, a
	 * build key -- has the flag unset, so it is charged in its own right,
	 * which is what keeps it visible to the scheduler.
	 */
	if ((vy_stmt_flags(stmt) & VY_STMT_MEM) == 0) {
		vy_stmt_add_flag(stmt, VY_STMT_MEM);
		mem->stat.uniq_bytes += tuple_size(stmt);
		mem->lsm_stat->uniq_bytes += tuple_size(stmt);
		mem->env->stat.uniq_bytes += tuple_size(stmt);
		vy_quota_acct(mem->env->quota_acct, consumer, tuple_size(stmt));
	}
}

/*
 * Only the count is adjusted; uniq_bytes is not. A statement can leave
 * the tree before its mem is dumped -- an upsert squashed by a REPLACE,
 * a multikey duplicate, a rolled-back write -- but its charge lives in
 * the mem that first saw it, which is not necessarily this one: a tuple
 * shared across the primary and secondary mems is charged by the
 * primary, so when it leaves a secondary mem the charge is elsewhere
 * and there is nothing here to release. Rather than track per-statement
 * ownership, the charge is released in bulk at vy_mem_delete, which
 * returns each mem's own uniq_bytes. The lag -- a charge outliving its
 * statement until the dump -- only over-counts acct.used, firing a dump
 * a little early, the safe direction.
 */
static void
vy_mem_unacct(struct vy_mem *mem, struct tuple *stmt)
{
	vy_stmt_counter_unacct_tuple(&mem->stat.count, stmt);
	vy_stmt_counter_unacct_tuple(&mem->lsm_stat->count, stmt);
	vy_stmt_counter_unacct_tuple(&mem->env->stat.count, stmt);
}

/**
 * Set VY_STMT_COUNTED on @a entry if it should contribute to the
 * per-type stats used by space:len(). Runs at insert (prepare)
 * time while the tree iterator is still at hand, so the older
 * lookup is O(1). The flag is then read at commit and at
 * squash-displace without any tree work.
 *
 * Flag accuracy relies on (a) the stmt's physical tree position
 * being stable between prepare and commit -- LSN is reassigned on
 * commit but the BPS tree is not resorted, so the stmt stays below
 * the same older neighbour; (b) in-place type changes preserving
 * the DELETE/non-DELETE distinction the predicate keys on (only
 * the squash UPSERT->REPLACE swap does this and is safe); (c)
 * prepared-write rollbacks being rare enough that the occasional
 * stale flag on a neighbour is within space:len()'s existing
 * approximation budget.
 */
static inline void
vy_mem_set_counted_flag(struct vy_mem *mem, struct vy_entry entry,
			struct vy_mem_tree_iterator *at)
{
	if (mem->stmt == NULL)
		return;
	struct vy_entry older = vy_mem_older_at(mem, at);
	if (!vy_mem_stmt_is_counted(vy_stmt_type(entry.stmt), older))
		return;
	vy_stmt_add_flag(entry.stmt, VY_STMT_COUNTED);
}

int
vy_mem_insert_upsert(struct vy_mem *mem, struct vy_entry entry,
		     enum vy_quota_consumer_type consumer,
		     struct vy_entry *prev)
{
	assert(vy_stmt_type(entry.stmt) == IPROTO_UPSERT);
	/* Check if the statement can be inserted in the vy_mem. */
	assert(entry.stmt->format_id == tuple_format_id(mem->format));
	struct vy_entry replaced = vy_entry_none();
	struct vy_mem_tree_iterator at;
	if (vy_mem_tree_insert_get_iterator(&mem->tree, entry, &replaced,
					    &at) != 0)
		return -1;
	assert(!vy_mem_tree_iterator_is_invalid(&at));
	assert(vy_entry_is_equal(entry,
		*vy_mem_tree_iterator_get_elem(&mem->tree, &at)));
	/*
	 * vy_tx_prepare squashes same-key writes within a txn and the
	 * PK has no multikey or deferred-delete paths, so an UPSERT
	 * never collides at (key, lsn). The squash path writes a
	 * REPLACE (not an UPSERT) and goes through vy_mem_insert.
	 */
	assert(replaced.stmt == NULL);
	vy_stmt_ref(entry.stmt);
	vy_mem_acct(mem, entry.stmt, consumer);
	vy_mem_set_counted_flag(mem, entry, &at);
	/*
	 * All iterators begin to see the new statement, and
	 * will be aborted in case of rollback.
	 */
	mem->version++;
	/*
	 * Get the previous version of this key. Return it to
	 * the caller for WW conflict detection, and use it
	 * below for n_upserts counting.
	 */
	vy_mem_tree_iterator_next(&mem->tree, &at);
	struct vy_entry *older = vy_mem_tree_iterator_get_elem(&mem->tree,
							       &at);
	if (older == NULL ||
	    vy_entry_compare(entry, *older, mem->cmp_def) != 0) {
		if (prev != NULL)
			*prev = vy_entry_none();
		return 0;
	}
	if (prev != NULL)
		*prev = *older;
	/*
	 * Update n_upserts if needed. Get the previous statement
	 * from the inserted one and if it has the same key, then
	 * increment n_upserts of the new statement until the
	 * predefined limit:
	 *
	 * UPSERT, n = 0
	 * UPSERT, n = 1,
	 *         ...
	 * UPSERT, n = threshold,
	 * UPSERT, n = threshold + 1,
	 * UPSERT, n = threshold + 1, all following ones have
	 *         ...                threshold + 1.
	 * These values are used by vy_lsm_commit_upsert to squash
	 * UPSERTs subsequence.
	 */
	if (vy_stmt_type(older->stmt) != IPROTO_UPSERT)
		return 0;
	uint8_t n_upserts = vy_stmt_n_upserts(older->stmt);
	/*
	 * Stop increment if the threshold is reached to avoid
	 * creation of multiple squashing tasks.
	 */
	if (n_upserts <= VY_UPSERT_THRESHOLD)
		n_upserts++;
	else
		assert(n_upserts == VY_UPSERT_INF);
	vy_stmt_set_n_upserts(entry.stmt, n_upserts);
	return 0;
}

int
vy_mem_insert(struct vy_mem *mem, struct vy_entry entry,
	      enum vy_quota_consumer_type consumer, struct vy_entry *prev)
{
	assert(vy_stmt_type(entry.stmt) != IPROTO_UPSERT);
	/* Check if the statement can be inserted in the vy_mem. */
	assert(vy_stmt_is_key(entry.stmt) ||
	       entry.stmt->format_id == tuple_format_id(mem->format));
	struct vy_entry replaced = vy_entry_none();
	struct vy_mem_tree_iterator at;
	if (vy_mem_tree_insert_get_iterator(&mem->tree, entry, &replaced,
					    &at) != 0)
		return -1;
	vy_stmt_ref(entry.stmt);
	vy_mem_acct(mem, entry.stmt, consumer);
	mem->version++;
	if (replaced.stmt != NULL) {
		/*
		 * PK: upsert squash (REPLACE displaces a committed
		 * UPSERT at the same (key, lsn)). SK: multikey tuple
		 * producing the same (key, lsn) twice. Net rows
		 * cancel; bytes adjust by the difference.
		 */
		assert(mem->stmt == NULL ||
		       (vy_stmt_type(entry.stmt) == IPROTO_REPLACE &&
			vy_stmt_type(replaced.stmt) == IPROTO_UPSERT));
		vy_mem_unacct(mem, replaced.stmt);
		vy_stmt_unref(replaced.stmt);
		if (prev != NULL)
			*prev = vy_entry_none();
		return 0;
	}
	vy_mem_set_counted_flag(mem, entry, &at);
	/*
	 * Return the previous version of this key in the mem
	 * (if any). Used by snapshot isolation to detect
	 * write-write conflicts as a side effect of the insert.
	 */
	if (prev != NULL) {
		*prev = vy_entry_none();
		vy_mem_tree_iterator_next(&mem->tree, &at);
		struct vy_entry *older =
			vy_mem_tree_iterator_get_elem(&mem->tree, &at);
		if (older != NULL &&
		    vy_entry_compare(entry, *older, mem->cmp_def) == 0)
			*prev = *older;
	}
	return 0;
}

void
vy_mem_commit_stmt(struct vy_mem *mem, struct vy_entry entry)
{
	int64_t lsn = vy_stmt_lsn(entry.stmt);
	/*
	 * Normally statement LSN grows monotonically,
	 * but not in case of building an index on an
	 * existing non-empty space. Hence use of MAX
	 * here.
	 */
	mem->dump_lsn = MAX(mem->dump_lsn, lsn);
	/*
	 * If we don't bump mem version after assigning LSN to
	 * a mem statement, a read iterator which uses
	 * committed_read_view and yields might not see it after
	 * yield finishes and return a stale tuple.
	 */
	mem->version++;
	/*
	 * The flag is cached on the statement, which is shared across
	 * the primary and secondary mems; gate on mem->stmt so only the
	 * primary bumps per-type stats.
	 */
	if (mem->stmt != NULL &&
	    (vy_stmt_flags(entry.stmt) & VY_STMT_COUNTED) != 0)
		vy_stmt_stat_acct(mem->stmt, vy_stmt_type(entry.stmt));
}

void
vy_mem_rollback_stmt(struct vy_mem *mem, struct vy_entry entry)
{
	/*
	 * A later vy_mem_insert may have already displaced this
	 * entry and dropped our ref; skip the decrement / unref in
	 * that case so we don't double-free.
	 */
	if (vy_mem_tree_delete(&mem->tree, entry) == 0) {
		mem->version++;
		vy_mem_unacct(mem, entry.stmt);
		vy_stmt_unref(entry.stmt);
	}
}

/* }}} vy_mem */

/* {{{ vy_mem_iterator support functions */

/**
 * Make a step in the iterator direction.
 * @retval 0 success
 * @retval 1 EOF
 */
static int
vy_mem_iterator_step(struct vy_mem_iterator *itr)
{
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	else
		vy_mem_tree_iterator_next(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
						   &itr->curr_pos);
	return 0;
}

/**
 * Return true if the current statement should be skipped.
 * Note, the function may update min_skipped_plsn.
 */
static inline bool
vy_mem_iterator_should_skip_curr(struct vy_mem_iterator *itr)
{
	struct tuple *stmt = itr->curr.stmt;
	if (vy_stmt_flags(stmt) & VY_STMT_SKIP_READ)
		return true;
	if (!itr->is_prepared_ok && vy_stmt_is_prepared(stmt)) {
		/*
		 * Skipping a prepared statement sends the reader to a
		 * read view (see min_skipped_plsn), which protects the
		 * reader's results -- but not its cache claims: the
		 * result is shadowed by the skipped statement and must
		 * not be cached as the latest, so the skip taints it
		 * like any skip of an invisible newer version.
		 */
		itr->is_stale = true;
		itr->min_skipped_plsn = MIN(itr->min_skipped_plsn,
					    vy_stmt_lsn(stmt));
		return true;
	}
	return false;
}

/**
 * Find next record with lsn <= itr->lsn record.
 * Current position must be at the beginning of serie of records with the
 * same key it terms of direction of iterator (i.e. left for GE, right for LE)
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_find_lsn(struct vy_mem_iterator *itr)
{
	/* Skip to the first statement visible in the read view. */
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	assert(vy_entry_is_equal(itr->curr,
		*vy_mem_tree_iterator_get_elem(&itr->mem->tree,
					       &itr->curr_pos)));
	struct key_def *cmp_def = itr->mem->cmp_def;
	while (vy_stmt_lsn(itr->curr.stmt) > (**itr->read_view).vlsn ||
	       vy_mem_iterator_should_skip_curr(itr)) {
		if (vy_stmt_lsn(itr->curr.stmt) > (**itr->read_view).vlsn)
			itr->is_stale = true;
		if (vy_mem_iterator_step(itr) != 0 ||
		    (itr->iterator_type == ITER_EQ &&
		     vy_entry_compare(itr->key, itr->curr, cmp_def))) {
			itr->curr = vy_entry_none();
			return 1;
		}
	}
	if (iterator_direction(itr->iterator_type) > 0)
		return 0;
	/*
	 * Since statements are sorted by LSN in descending order,
	 * for LE/LT iterator we must skip to the statement with
	 * max LSN visible in the read view.
	 */
	struct vy_mem_tree_iterator prev_pos = itr->curr_pos;
	vy_mem_tree_iterator_prev(&itr->mem->tree, &prev_pos);
	if (vy_mem_tree_iterator_is_invalid(&prev_pos)) {
		/* No more statements. */
		return 0;
	}
	struct vy_entry prev;
	prev = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &prev_pos);
	if (vy_stmt_lsn(prev.stmt) > (**itr->read_view).vlsn ||
	    vy_entry_compare(itr->curr, prev, cmp_def) != 0) {
		/*
		 * The next statement is either invisible in
		 * the read view or for another key.
		 */
		return 0;
	}
	/*
	 * We could iterate linearly until a statement invisible
	 * in the read view is found, but there's a good chance
	 * that this key is frequently updated and so the iteration
	 * is going to take long. So instead we look it up - it's
	 * pretty cheap anyway.
	 */
	struct vy_mem_tree_key tree_key;
	tree_key.entry = itr->curr;
	tree_key.lsn = (**itr->read_view).vlsn;
	itr->curr_pos = vy_mem_tree_lower_bound(&itr->mem->tree,
						&tree_key, NULL);
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	itr->curr = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
						   &itr->curr_pos);
	while (vy_mem_iterator_should_skip_curr(itr)) {
		vy_mem_tree_iterator_next(&itr->mem->tree, &itr->curr_pos);
		assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
		itr->curr = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
							   &itr->curr_pos);
	}
	return 0;
}

/**
 * Position the iterator to the first statement satisfying the
 * iterator search criteria and following the given key (pass
 * NULL to start iteration).
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_seek(struct vy_mem_iterator *itr, struct vy_entry last)
{
	itr->stat->lookup++;
	itr->search_started = true;
	itr->version = itr->mem->version;
	itr->curr = vy_entry_none();

	struct vy_entry key = itr->key;
	enum iterator_type iterator_type = itr->iterator_type;
	if (last.stmt != NULL) {
		key = last;
		iterator_type = iterator_direction(itr->iterator_type) > 0 ?
				ITER_GT : ITER_LT;
	}

	bool exact = false;
	struct vy_mem_tree_key tree_key;
	tree_key.entry = key;
	/* (lsn == INT64_MAX - 1) means that lsn is ignored in comparison */
	tree_key.lsn = INT64_MAX - 1;
	if (!vy_stmt_is_empty_key(key.stmt)) {
		if (iterator_type == ITER_LE || iterator_type == ITER_GT) {
			itr->curr_pos =
				vy_mem_tree_upper_bound(&itr->mem->tree,
							&tree_key, &exact);
		} else {
			assert(iterator_type == ITER_EQ ||
			       iterator_type == ITER_GE ||
			       iterator_type == ITER_LT);
			itr->curr_pos =
				vy_mem_tree_lower_bound(&itr->mem->tree,
							&tree_key, &exact);
		}
	} else if (iterator_type == ITER_LE) {
		itr->curr_pos = vy_mem_tree_invalid_iterator();
	} else {
		assert(iterator_type == ITER_GE);
		itr->curr_pos = vy_mem_tree_first(&itr->mem->tree);
	}

	if (iterator_type == ITER_LT || iterator_type == ITER_LE)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
						   &itr->curr_pos);
	if (itr->iterator_type == ITER_EQ &&
	    ((last.stmt == NULL && !exact) ||
	     (last.stmt != NULL &&
	      vy_entry_compare(itr->key, itr->curr, itr->mem->cmp_def) != 0))) {
		itr->curr = vy_entry_none();
		return 1;
	}
	return vy_mem_iterator_find_lsn(itr);
}

/* }}} vy_mem_iterator support functions */

/* {{{ vy_mem_iterator API implementation */

void
vy_mem_iterator_open(struct vy_mem_iterator *itr, struct vy_mem_iterator_stat *stat,
		     struct vy_mem *mem, enum iterator_type iterator_type,
		     struct vy_entry key, const struct vy_read_view **rv,
		     bool is_prepared_ok)
{
	itr->stat = stat;

	assert(key.stmt != NULL);
	itr->mem = mem;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->read_view = rv;

	itr->curr_pos = vy_mem_tree_invalid_iterator();
	itr->curr = vy_entry_none();

	itr->search_started = false;
	itr->is_prepared_ok = is_prepared_ok;
	itr->min_skipped_plsn = INT64_MAX;
	itr->is_stale = false;
}

/*
 * Find the next record with different key as current and visible lsn.
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_key(struct vy_mem_iterator *itr)
{
	if (!itr->search_started)
		return vy_mem_iterator_seek(itr, vy_entry_none());
	if (itr->curr.stmt == NULL) /* End of search. */
		return 1;
	assert(itr->mem->version == itr->version);
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	assert(vy_entry_is_equal(itr->curr,
		*vy_mem_tree_iterator_get_elem(&itr->mem->tree,
					       &itr->curr_pos)));
	struct key_def *cmp_def = itr->mem->cmp_def;

	struct vy_entry prev = itr->curr;
	if (vy_mem_iterator_step(itr) != 0) {
		itr->curr = vy_entry_none();
		return 1;
	}
	/*
	 * If we are still on the same key after making a step,
	 * there's a good chance there's a lot of statements
	 * for this key so instead of iterating further we simply
	 * look up the next key - it's pretty cheap anyway.
	 */
	if (vy_entry_compare(prev, itr->curr, cmp_def) == 0)
		return vy_mem_iterator_seek(itr, itr->curr);

	if (itr->iterator_type == ITER_EQ &&
	    vy_entry_compare(itr->key, itr->curr, cmp_def) != 0) {
		itr->curr = vy_entry_none();
		return 1;
	}
	return vy_mem_iterator_find_lsn(itr);
}

/*
 * Find next (lower, older) record with the same key as current
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_lsn(struct vy_mem_iterator *itr)
{
	assert(itr->search_started);
	if (itr->curr.stmt == NULL) /* End of search. */
		return 1;
	assert(itr->mem->version == itr->version);
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	assert(vy_entry_is_equal(itr->curr,
		*vy_mem_tree_iterator_get_elem(&itr->mem->tree,
					       &itr->curr_pos)));
	struct key_def *cmp_def = itr->mem->cmp_def;

	struct vy_mem_tree_iterator next_pos = itr->curr_pos;
next:
	vy_mem_tree_iterator_next(&itr->mem->tree, &next_pos);
	if (vy_mem_tree_iterator_is_invalid(&next_pos))
		return 1; /* EOF */

	struct vy_entry next;
	next = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &next_pos);
	if (vy_entry_compare(itr->curr, next, cmp_def) != 0)
		return 1;

	itr->curr_pos = next_pos;
	itr->curr = next;
	if (vy_stmt_flags(itr->curr.stmt) & VY_STMT_SKIP_READ)
		goto next;
	return 0;
}

/**
 * Append statements for the current key to a statement history
 * until a terminal statement is found. Returns 0 on success, -1
 * on memory allocation error.
 */
static NODISCARD int
vy_mem_iterator_get_history(struct vy_mem_iterator *itr,
			    struct vy_history *history)
{
	do {
		vy_stmt_counter_acct_tuple(&itr->stat->get, itr->curr.stmt);
		if (vy_history_append_stmt(history, itr->curr) != 0)
			return -1;
		if (vy_history_is_terminal(history))
			break;
	} while (vy_mem_iterator_next_lsn(itr) == 0);
	return 0;
}

NODISCARD int
vy_mem_iterator_next(struct vy_mem_iterator *itr,
		     struct vy_history *history)
{
	vy_history_cleanup(history);
	if (vy_mem_iterator_next_key(itr) == 0)
		return vy_mem_iterator_get_history(itr, history);
	return 0;
}

NODISCARD int
vy_mem_iterator_skip(struct vy_mem_iterator *itr, struct vy_entry last,
		     struct vy_history *history)
{
	assert(!itr->search_started || itr->version == itr->mem->version);

	/*
	 * Check if the iterator is already positioned
	 * at the statement following last.
	 */
	if (itr->search_started &&
	    (itr->curr.stmt == NULL || last.stmt == NULL ||
	     iterator_direction(itr->iterator_type) *
	     vy_entry_compare(itr->curr, last, itr->mem->cmp_def) > 0))
		return 0;

	vy_history_cleanup(history);
	if (vy_mem_iterator_seek(itr, last) == 0)
		return vy_mem_iterator_get_history(itr, history);
	return 0;
}

NODISCARD int
vy_mem_iterator_restore(struct vy_mem_iterator *itr, struct vy_entry last,
			struct vy_history *history)
{
	if (!itr->search_started || itr->version == itr->mem->version)
		return 0;

	vy_mem_iterator_seek(itr, last);

	vy_history_cleanup(history);
	if (itr->curr.stmt != NULL &&
	    vy_mem_iterator_get_history(itr, history) != 0)
		return -1;
	return 1;
}

void
vy_mem_iterator_close(struct vy_mem_iterator *itr)
{
	TRASH(itr);
}

static NODISCARD int
vy_mem_stream_next(struct vy_stmt_stream *virt_stream, struct vy_entry *ret)
{
	assert(virt_stream->iface->next == vy_mem_stream_next);
	struct vy_mem_stream *stream = (struct vy_mem_stream *)virt_stream;

	struct vy_entry *res =
		vy_mem_tree_iterator_get_elem(&stream->mem->tree,
					      &stream->curr_pos);
	if (res == NULL) {
		*ret = vy_entry_none();
	} else {
		*ret = *res;
		vy_mem_tree_iterator_next(&stream->mem->tree,
					  &stream->curr_pos);
	}
	return 0;
}

static const struct vy_stmt_stream_iface vy_mem_stream_iface = {
	.start = NULL,
	.next = vy_mem_stream_next,
	.stop = NULL,
	.close = NULL
};

void
vy_mem_stream_open(struct vy_mem_stream *stream, struct vy_mem *mem)
{
	stream->base.iface = &vy_mem_stream_iface;
	stream->mem = mem;
	stream->curr_pos = vy_mem_tree_first(&mem->tree);
}

/* }}} vy_mem_iterator API implementation */
