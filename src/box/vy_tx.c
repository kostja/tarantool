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
#include "vy_tx.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <small/mempool.h>
#include <small/rlist.h>

#include "diag.h"
#include "errcode.h"
#include "fiber.h"
#include "iproto_constants.h"
#include "iterator_type.h"
#include "salad/stailq.h"
#include "schema.h" /* space_cache_version */
#include "session.h"
#include "space.h"
#include "trigger.h"
#include "trivia/util.h"
#include "tuple.h"
#include "vy_lsm.h"
#include "vy_mem.h"
#include "vy_stat.h"
#include "vy_stmt.h"
#include "vy_upsert.h"
#include "vy_history.h"
#include "vy_read_set.h"
#include "vy_read_view.h"
#include "vy_point_lookup.h"

int
write_set_cmp(struct txv *a, struct txv *b)
{
	int rc = a->lsm < b->lsm ? -1 : a->lsm > b->lsm;
	if (rc == 0)
		return vy_entry_compare(a->entry, b->entry, a->lsm->cmp_def);
	return rc;
}

int
write_set_key_cmp(struct write_set_key *a, struct txv *b)
{
	int rc = a->lsm < b->lsm ? -1 : a->lsm > b->lsm;
	if (rc == 0)
		return vy_entry_compare(a->entry, b->entry, a->lsm->cmp_def);
	return rc;
}

/**
 * Initialize an instance of a global read view.
 * To be used exclusively by the transaction manager.
 */
static void
vy_global_read_view_create(struct vy_read_view *rv, int64_t lsn)
{
	rlist_create(&rv->in_read_views);
	/*
	 * By default, the transaction is assumed to be
	 * read-write, and it reads the latest changes of all
	 * prepared transactions. This makes it possible to
	 * use the tuple cache in it.
	 */
	rv->vlsn = lsn;
	rv->refs = 0;
}

struct vy_tx_manager *
vy_tx_manager_new(void)
{
	struct vy_tx_manager *xm = calloc(1, sizeof(*xm));
	if (xm == NULL) {
		diag_set(OutOfMemory, sizeof(*xm),
			 "malloc", "struct vy_tx_manager");
		return NULL;
	}

	rlist_create(&xm->writers);
	rlist_create(&xm->prepared);
	rlist_create(&xm->read_views);
	rlist_create(&xm->snapshot_active);
	fiber_cond_create(&xm->read_view_cond);
	vy_global_read_view_create((struct vy_read_view *)&xm->global_read_view,
				   INT64_MAX);
	xm->p_global_read_view = &xm->global_read_view;
	vy_global_read_view_create((struct vy_read_view *)&xm->committed_read_view,
				   MAX_LSN - 1);
	xm->p_committed_read_view = &xm->committed_read_view;

	struct slab_cache *slab_cache = cord_slab_cache();
	mempool_create(&xm->tx_mempool, slab_cache, sizeof(struct vy_tx));
	mempool_create(&xm->txv_mempool, slab_cache, sizeof(struct txv));
	mempool_create(&xm->read_interval_mempool, slab_cache,
		       sizeof(struct vy_read_interval));
	mempool_create(&xm->read_view_mempool, slab_cache,
		       sizeof(struct vy_read_view));
	return xm;
}

void
vy_tx_manager_delete(struct vy_tx_manager *xm)
{
	fiber_cond_destroy(&xm->read_view_cond);
	mempool_destroy(&xm->read_view_mempool);
	mempool_destroy(&xm->read_interval_mempool);
	mempool_destroy(&xm->txv_mempool);
	mempool_destroy(&xm->tx_mempool);
	free(xm);
}

size_t
vy_tx_manager_mem_used(struct vy_tx_manager *xm)
{
	struct mempool_stats mstats;
	size_t ret = 0;

	ret += xm->write_set_size + xm->read_set_size;
	mempool_stats(&xm->tx_mempool, &mstats);
	ret += mstats.totals.used;
	mempool_stats(&xm->txv_mempool, &mstats);
	ret += mstats.totals.used;
	mempool_stats(&xm->read_interval_mempool, &mstats);
	ret += mstats.totals.used;
	mempool_stats(&xm->read_view_mempool, &mstats);
	ret += mstats.totals.used;

	return ret;
}

struct vy_read_view *
vy_tx_manager_read_view(struct vy_tx_manager *xm, int64_t plsn)
{
	assert(plsn >= MAX_LSN);
	/* Look up the last read view with lsn less than the given one. */
	struct vy_read_view *rv;
	rlist_foreach_entry_reverse(rv, &xm->read_views, in_read_views) {
		if (plsn > rv->vlsn)
			break;
	}
	bool rv_exists = !rlist_entry_is_head(rv, &xm->read_views,
					      in_read_views);
	/* Look up the last prepared tx with lsn less than the given one. */
	struct vy_tx *tx;
	rlist_foreach_entry_reverse(tx, &xm->prepared, in_prepared) {
		if (plsn > MAX_LSN + tx->psn)
			break;
	}
	bool tx_exists = !rlist_entry_is_head(tx, &xm->prepared, in_prepared);
	/*
	 * Check if the last read view can be reused. Reference
	 * and return it if it's the case.
	 */
	if (rv_exists) {
		if ((!tx_exists && rv->vlsn == xm->lsn) ||
		    (tx_exists && rv->vlsn == MAX_LSN + tx->psn)) {
			rv->refs++;
			return rv;
		}
	}
	/*
	 * Allocate a new read view and insert it into the read view list
	 * preserving the order.
	 */
	struct vy_read_view *prev_rv = rv;
	rv = xmempool_alloc(&xm->read_view_mempool);
	/*
	 * Save the old read view to destroy after insertion.
	 * Destroying before insertion would free prev_rv if
	 * prev_rv == old_rv and refs == 1.
	 */
	struct vy_read_view *old_rv = NULL;
	if (tx_exists) {
		rv->vlsn = MAX_LSN + tx->psn;
		old_rv = tx->read_view;
		tx->read_view = rv;
		rv->refs = 2;
	} else {
		rv->vlsn = xm->lsn;
		rv->refs = 1;
	}
	rlist_add_entry(&prev_rv->in_read_views, rv, in_read_views);
	if (old_rv != NULL)
		vy_tx_manager_destroy_read_view(xm, old_rv);
	return rv;
}

void
vy_tx_manager_destroy_read_view(struct vy_tx_manager *xm,
                                struct vy_read_view *rv)
{
	if (rv == xm->p_global_read_view)
		return;
	assert(rv->refs);
	if (--rv->refs == 0) {
		rlist_del_entry(rv, in_read_views);
		mempool_free(&xm->read_view_mempool, rv);
	}
}

static struct txv *
txv_new(struct vy_tx *tx, struct vy_lsm *lsm, struct vy_entry entry)
{
	struct vy_tx_manager *xm = tx->xm;
	struct txv *v = xmempool_alloc(&xm->txv_mempool);
	v->lsm = lsm;
	vy_lsm_ref(v->lsm);
	v->mem = NULL;
	v->entry = entry;
	tuple_ref(entry.stmt);
	v->region_stmt = NULL;
	v->tx = tx;
	v->is_first_insert = false;
	v->is_nop = false;
	v->is_overwritten = false;
	v->overwritten = NULL;
	xm->write_set_size += tuple_size(entry.stmt);
	vy_stmt_counter_acct_tuple(&lsm->stat.txw.count, entry.stmt);
	return v;
}

static void
txv_delete(struct txv *v)
{
	struct vy_tx_manager *xm = v->tx->xm;
	xm->write_set_size -= tuple_size(v->entry.stmt);
	vy_stmt_counter_unacct_tuple(&v->lsm->stat.txw.count, v->entry.stmt);
	tuple_unref(v->entry.stmt);
	vy_lsm_unref(v->lsm);
	mempool_free(&xm->txv_mempool, v);
}

/**
 * Account a read interval in transaction manager stats.
 */
static void
vy_read_interval_acct(struct vy_read_interval *interval)
{
	struct vy_tx_manager *xm = interval->tx->xm;
	xm->read_set_size += tuple_size(interval->left.stmt);
	if (interval->left.stmt != interval->right.stmt)
		xm->read_set_size += tuple_size(interval->right.stmt);
}

/**
 * Unaccount a read interval in transaction manager stats.
 */
static void
vy_read_interval_unacct(struct vy_read_interval *interval)
{
	struct vy_tx_manager *xm = interval->tx->xm;
	xm->read_set_size -= tuple_size(interval->left.stmt);
	if (interval->left.stmt != interval->right.stmt)
		xm->read_set_size -= tuple_size(interval->right.stmt);
}

static struct vy_read_interval *
vy_read_interval_new(struct vy_tx *tx, struct vy_lsm *lsm,
		     struct vy_entry left, bool left_belongs,
		     struct vy_entry right, bool right_belongs)
{
	struct vy_tx_manager *xm = tx->xm;
	struct vy_read_interval *interval;
	interval = xmempool_alloc(&xm->read_interval_mempool);
	interval->tx = tx;
	vy_lsm_ref(lsm);
	interval->lsm = lsm;
	tuple_ref(left.stmt);
	interval->left = left;
	interval->left_belongs = left_belongs;
	tuple_ref(right.stmt);
	interval->right = right;
	interval->right_belongs = right_belongs;
	interval->subtree_last = NULL;
	vy_read_interval_acct(interval);
	return interval;
}

static void
vy_read_interval_delete(struct vy_read_interval *interval)
{
	struct vy_tx_manager *xm = interval->tx->xm;
	vy_read_interval_unacct(interval);
	vy_lsm_unref(interval->lsm);
	tuple_unref(interval->left.stmt);
	tuple_unref(interval->right.stmt);
	mempool_free(&xm->read_interval_mempool, interval);
}

static struct vy_read_interval *
vy_tx_read_set_free_cb(vy_tx_read_set_t *read_set,
		       struct vy_read_interval *interval, void *arg)
{
	(void)arg;
	(void)read_set;
	vy_lsm_read_set_remove(&interval->lsm->read_set, interval);
	vy_read_interval_delete(interval);
	return NULL;
}

void
vy_tx_create(struct vy_tx_manager *xm, struct vy_tx *tx)
{
	tx->last_stmt_space = NULL;
	stailq_create(&tx->log);
	write_set_new(&tx->write_set);
	tx->write_set_version = 0;
	tx->write_size = 0;
	tx->xm = xm;
	tx->isolation = TXN_ISOLATION_READ_CONFIRMED;
	tx->state = VINYL_TX_READY;
	tx->is_applier_session = false;
	tx->read_view = (struct vy_read_view *)xm->p_global_read_view;
	vy_tx_read_set_new(&tx->read_set);
	tx->psn = 0;
	rlist_create(&tx->on_destroy);
	rlist_create(&tx->in_writers);
	rlist_create(&tx->in_prepared);
	rlist_create(&tx->in_snapshot);
}

void
vy_tx_destroy(struct vy_tx *tx)
{
	assert(rlist_empty(&tx->in_prepared));

	trigger_run(&tx->on_destroy, NULL);
	trigger_destroy(&tx->on_destroy);

	vy_tx_manager_destroy_read_view(tx->xm, tx->read_view);

	struct txv *v, *tmp;
	stailq_foreach_entry_safe(v, tmp, &tx->log, next_in_log)
		txv_delete(v);

	vy_tx_read_set_iter(&tx->read_set, NULL, vy_tx_read_set_free_cb, NULL);
	rlist_del_entry(tx, in_writers);
	rlist_del_entry(tx, in_snapshot);
}

/**
 * Mark a transaction as aborted and release its read view
 * so that the LSM tree can compact old data. Without the
 * release, aborted TXs would pin their read view until
 * the user calls rollback.
 */
static void
vy_tx_abort(struct vy_tx *tx)
{
	assert(tx->state == VINYL_TX_READY);
	tx->state = VINYL_TX_ABORT;
	tx->xm->stat.conflict++;
	vy_tx_manager_destroy_read_view(tx->xm, tx->read_view);
	tx->read_view = (struct vy_read_view *)tx->xm->p_global_read_view;
	rlist_del_entry(tx, in_snapshot);
}

/** Return true if the transaction is read-only. */
static bool
vy_tx_is_ro(struct vy_tx *tx)
{
	return write_set_empty(&tx->write_set);
}

/** Return true if the transaction is in read view. */
static bool
vy_tx_is_in_read_view(struct vy_tx *tx)
{
	return tx->read_view->vlsn != INT64_MAX;
}

/**
 * Return true if this is a snapshot TX that has a read view
 * assigned (i.e., it has read something and is no longer blind).
 */
static bool
vy_tx_is_in_snapshot(struct vy_tx *tx)
{
	return tx->isolation == TXN_ISOLATION_SNAPSHOT &&
	       vy_tx_is_in_read_view(tx);
}

void
vy_tx_send_to_read_view(struct vy_tx *tx, int64_t plsn)
{
	assert(plsn >= MAX_LSN);
	assert(tx->state == VINYL_TX_READY);
	/* Snapshot TXs don't register in read_set. */
	assert(tx->isolation != TXN_ISOLATION_SNAPSHOT);
	if (tx->read_view->vlsn < plsn)
		return;
	if (!vy_tx_is_ro(tx)) {
		vy_tx_abort(tx);
		return;
	}
	struct vy_tx_manager *xm = tx->xm;
	struct vy_read_view *rv = vy_tx_manager_read_view(xm, plsn);
	vy_tx_manager_destroy_read_view(xm, tx->read_view);
	tx->read_view = rv;
}

/**
 * Send to read view all transactions that are reading key @v
 * modified by transaction @tx and abort all transactions that are modifying it.
 */
static void
vy_tx_send_readers_to_read_view(struct vy_tx *tx, struct txv *v)
{
	struct vy_tx_conflict_iterator it;
	vy_tx_conflict_iterator_init(&it, &v->lsm->read_set, v->entry);
	struct vy_tx *abort;
	while ((abort = vy_tx_conflict_iterator_next(&it)) != NULL) {
		/* Don't abort self. */
		if (abort == tx)
			continue;
		/* Abort only active TXs */
		if (abort->state != VINYL_TX_READY)
			continue;
		/* Snapshot TXs don't register in read_set. */
		assert(abort->isolation != TXN_ISOLATION_SNAPSHOT);
		vy_tx_send_to_read_view(abort, INT64_MAX);
	}
}

/**
 * Abort all transaction that are reading key @v modified
 * by transaction @tx.
 */
static void
vy_tx_abort_readers(struct vy_tx *tx, struct txv *v)
{
	struct vy_tx_conflict_iterator it;
	vy_tx_conflict_iterator_init(&it, &v->lsm->read_set, v->entry);
	struct vy_tx *abort;
	while ((abort = vy_tx_conflict_iterator_next(&it)) != NULL) {
		/* Don't abort self. */
		if (abort == tx)
			continue;
		/* Abort only active TXs */
		if (abort->state != VINYL_TX_READY)
			continue;
		vy_tx_abort(abort);
	}
}

struct vy_tx *
vy_tx_begin(struct vy_tx_manager *xm, enum txn_isolation_level isolation)
{
	assert(isolation < txn_isolation_level_MAX &&
	       isolation != TXN_ISOLATION_DEFAULT);
	struct vy_tx *tx = xmempool_alloc(&xm->tx_mempool);
	vy_tx_create(xm, tx);

	struct session *session = fiber_get_session(fiber());
	if (session != NULL && session->type == SESSION_TYPE_APPLIER)
		tx->is_applier_session = true;

	tx->isolation = isolation;
	/*
	 * Snapshot TXs start with the global read view. The
	 * read view is assigned lazily on the first read via
	 * vy_tx_read_view(). Write-only TXs never get a read
	 * view, which lets blind writes skip WW conflict
	 * detection entirely. The TX is added to
	 * snapshot_active lazily too (when the read view is
	 * assigned) to keep the list sorted by vlsn.
	 */
	return tx;
}

const struct vy_read_view **
vy_tx_read_view(struct vy_tx *tx)
{
	if (tx->isolation == TXN_ISOLATION_SNAPSHOT &&
	    tx->read_view->vlsn == INT64_MAX) {
		struct vy_tx_manager *xm = tx->xm;
		tx->read_view = vy_tx_manager_read_view(xm, INT64_MAX);
		/*
		 * Append to tail: xm->lsn is monotonic, so the
		 * list stays sorted by vlsn. This ordering is
		 * relied upon by
		 * vy_tx_manager_check_concurrent_write.
		 */
		rlist_add_tail_entry(&xm->snapshot_active, tx,
				     in_snapshot);
	}
	return (const struct vy_read_view **)&tx->read_view;
}

/**
 * Try to generate a deferred DELETE statement on tx commit.
 *
 * This function is supposed to be called for a primary index
 * statement which was executed without deletion of the overwritten
 * tuple from secondary indexes. It looks up the overwritten tuple
 * in memory and, if found, produces the deferred DELETEs and
 * inserts them into the transaction log.
 *
 * Generating DELETEs before committing a transaction rather than
 * postponing it to dump isn't just an optimization. The point is
 * that we can't generate deferred DELETEs during dump, because
 * if we run out of memory, we won't be able to schedule another
 * dump to free some.
 *
 * Affects @tx->log, @v->entry.
 *
 * Returns 0 on success, -1 on memory allocation error.
 */
static int
vy_tx_handle_deferred_delete(struct vy_tx *tx, struct txv *v)
{
	struct vy_lsm *pk = v->lsm;
	struct tuple *stmt = v->entry.stmt;
	uint8_t flags = vy_stmt_flags(stmt);

	assert(pk->index_id == 0);
	assert(flags & VY_STMT_DEFERRED_DELETE);

	struct space *space = space_cache_find(pk->space_id);
	if (space == NULL) {
		/*
		 * Space was dropped while transaction was
		 * in progress. Nothing to do.
		 */
		return 0;
	}

	/* Look up the tuple overwritten by this statement. */
	struct vy_entry overwritten;
	if (vy_point_lookup_mem(pk, &tx->xm->p_global_read_view,
				v->entry, &overwritten) != 0)
		return -1;

	if (overwritten.stmt == NULL) {
		/*
		 * Nothing's found, but there still may be
		 * matching statements stored on disk so we
		 * have to defer generation of DELETE until
		 * compaction.
		 */
		return 0;
	}

	/*
	 * If a terminal statement is found, we can produce
	 * DELETE right away so clear the flag now.
	 */
	vy_stmt_set_flags(stmt, flags & ~VY_STMT_DEFERRED_DELETE);

	if (vy_stmt_type(overwritten.stmt) == IPROTO_DELETE) {
		/* The tuple's already deleted, nothing to do. */
		tuple_unref(overwritten.stmt);
		return 0;
	}

	struct tuple *delete_stmt;
	delete_stmt = vy_stmt_new_surrogate_delete(pk->mem_format,
						   overwritten.stmt);
	tuple_unref(overwritten.stmt);
	if (delete_stmt == NULL)
		return -1;

	if (vy_stmt_type(stmt) == IPROTO_DELETE) {
		/*
		 * Since primary and secondary indexes of the
		 * same space share in-memory statements, we
		 * need to use the new DELETE in the primary
		 * index, because the original DELETE doesn't
		 * contain secondary key parts.
		 */
		tx->xm->write_set_size -= tuple_size(stmt);
		tx->xm->write_set_size += tuple_size(delete_stmt);
		vy_stmt_counter_acct_tuple(&pk->stat.txw.count, delete_stmt);
		vy_stmt_counter_unacct_tuple(&pk->stat.txw.count, stmt);
		v->entry.stmt = delete_stmt;
		tuple_ref(delete_stmt);
		tuple_unref(stmt);
		stmt = delete_stmt;
	}

	/*
	 * Make DELETE statements for secondary indexes and
	 * insert them into the transaction log.
	 */
	for (uint32_t i = 1; i < space->index_count; i++) {
		struct vy_lsm *lsm = vy_lsm(space->index[i]);
		struct vy_entry entry;
		vy_stmt_foreach_entry(entry, delete_stmt, lsm->cmp_def) {
			/*
			 * If there's a statement in the transaction write set
			 * with the same key and it hasn't been overwritten by
			 * another statement, we have the following scenarios:
			 *
			 *  1. It's a DELETE. Then it must have been generated
			 *     by some UPDATE or UPSERT statement. Unless it's
			 *     a no-op, we have nothing to do.
			 *
			 *  2. It's a REPLACE. Then it must be a REPLACE
			 *     generated by the statement we're currently
			 *     processing (must be a REPLACE, too), which
			 *     happens not to update the secondary index key
			 *     parts. We must not generate a DELETE for it,
			 *     otherwise we'd lose the secondary key, but
			 *     we may skip it because it's essentially a no-op,
			 *     see vy_tx_set_entry().
			 */
			struct txv *other = write_set_search_key(&tx->write_set,
								 lsm, entry);
			if (other != NULL && !other->is_overwritten) {
				if (vy_stmt_type(other->entry.stmt) ==
				    IPROTO_DELETE) {
					if (!other->is_nop)
						continue;
				} else {
					assert(vy_stmt_type(stmt) ==
					       IPROTO_REPLACE);
					other->is_nop = true;
					continue;
				}
			}
			struct txv *delete_txv = txv_new(tx, lsm, entry);
			stailq_insert_entry(&tx->log, delete_txv, v,
					    next_in_log);
		}
	}
	tuple_unref(delete_stmt);
	return 0;
}

/**
 * Check if a vy_mem contains an entry matching @a entry by
 * @a key_def with LSN >= min_lsn (derived from @a tx's read
 * view). Used to detect write-write conflicts at prepare time
 * and at dump completion.
 *
 * For PK, pass cmp_def as key_def (exact full-key match).
 * For unique SK, pass key_def (SK parts only) -- the function
 * handles different PK values by checking entries on both
 * sides of the search position. The caller must skip NULL
 * keys (NULLs never conflict).
 *
 * Entries with the TX's own pseudo-LSN are skipped (to avoid
 * false self-conflicts at prepare time). For dump-completion
 * and DML-time checks the TX's entries are not in the mem, so
 * the exclusion is a no-op.
 */
static bool
vy_mem_check_concurrent_write(struct vy_mem *mem, struct vy_tx *tx,
			      struct vy_entry entry, struct key_def *key_def)
{
	int64_t min_lsn = tx->read_view->vlsn + 1;
	int64_t exclude_lsn = tx->psn > 0 ? MAX_LSN + tx->psn : 0;
	struct vy_mem_tree_key tree_key;
	tree_key.entry = entry;
	tree_key.lsn = INT64_MAX;
	struct vy_mem_tree_iterator itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, NULL);
	/*
	 * The tree sorts by (cmp_def, -LSN). lower_bound finds
	 * the first entry >= {key, INT64_MAX}, which is the
	 * highest-LSN entry for entries with cmp_def >= ours.
	 *
	 * For PK (key_def == cmp_def), there is only one group
	 * of entries with the same key, so a single check of the
	 * highest-LSN entry at the lower_bound position suffices.
	 *
	 * For unique SK (key_def != cmp_def), multiple PKs can
	 * share the same SK value (stale entries linger after
	 * insert/delete/insert cycles). Entries with the same SK
	 * but different PKs form separate groups in cmp_def
	 * order. Scan forward through all groups with PK >= ours,
	 * then backward through groups with PK < ours.
	 */
	struct vy_mem_tree_iterator fwd = itr;
	for (; !vy_mem_tree_iterator_is_invalid(&fwd);
	     vy_mem_tree_iterator_next(&mem->tree, &fwd)) {
		struct vy_entry *found =
			vy_mem_tree_iterator_get_elem(&mem->tree, &fwd);
		if (vy_entry_compare(entry, *found, key_def) != 0)
			break;
		int64_t lsn = vy_stmt_lsn(found->stmt);
		if (lsn >= min_lsn && lsn != exclude_lsn)
			return true;
	}
	if (key_def == mem->cmp_def)
		return false;
	for (vy_mem_tree_iterator_prev(&mem->tree, &itr);
	     !vy_mem_tree_iterator_is_invalid(&itr);
	     vy_mem_tree_iterator_prev(&mem->tree, &itr)) {
		struct vy_entry *found =
			vy_mem_tree_iterator_get_elem(&mem->tree, &itr);
		if (vy_entry_compare(entry, *found, key_def) != 0)
			break;
		int64_t lsn = vy_stmt_lsn(found->stmt);
		if (lsn >= min_lsn && lsn != exclude_lsn)
			return true;
	}
	return false;
}

/**
 * Check sealed vy_mems of @a lsm for a concurrent write to the
 * same key as @a entry. Returns true if a conflict is found.
 *
 * Sealed mems are scanned newest-first. Once an unpinned mem
 * with dump_lsn < min_lsn is found, scanning stops: WAL FIFO
 * guarantees all older sealed mems are fully resolved too, so
 * dump_lsn < min_lsn for all older mems.
 */
bool
vy_lsm_check_concurrent_write_mem(struct vy_lsm *lsm, struct vy_tx *tx,
				  struct vy_entry entry,
				  struct key_def *key_def)
{
	int64_t min_lsn = tx->read_view->vlsn + 1;
	struct vy_mem *mem;
	rlist_foreach_entry(mem, &lsm->sealed, in_sealed) {
		if (mem->pin_count == 0 &&
		    mem->dump_lsn >= 0 && mem->dump_lsn < min_lsn)
			break;
		if (vy_mem_check_concurrent_write(mem, tx, entry, key_def))
			return true;
	}
	return false;
}

/**
 * Check if @a tx has a write-write conflict with any of the
 * sealed mems of @a lsm that are eligible for dump (generation
 * <= @a dump_generation). Returns true if a conflict is found.
 */
static bool
vy_tx_manager_check_active_tx(struct vy_tx *tx, struct vy_lsm *lsm,
			      struct key_def *key,
			      int64_t dump_generation)
{
	struct txv *v;
	stailq_foreach_entry(v, &tx->log, next_in_log) {
		if (v->lsm != lsm)
			continue;
		/* NULL unique SK keys never conflict. */
		int multikey_idx = lsm->cmp_def->is_multikey ?
				   (int)v->entry.hint : MULTIKEY_NONE;
		if (lsm->index_id > 0 && key->is_nullable &&
		    tuple_key_contains_null(v->entry.stmt, key,
					    multikey_idx))
			continue;
		struct vy_mem *mem;
		rlist_foreach_entry(mem, &lsm->sealed, in_sealed) {
			if (mem->generation > dump_generation)
				continue;
			assert(mem->pin_count == 0);
			if (vy_mem_check_concurrent_write(mem, tx,
							  v->entry, key))
				return true;
		}
	}
	return false;
}

void
vy_tx_manager_check_concurrent_write(struct vy_tx_manager *xm,
				     struct vy_lsm *lsm,
				     int64_t dump_lsn,
				     int64_t dump_generation)
{
	struct key_def *key;
	if (lsm->index_id == 0)
		key = lsm->cmp_def;
	else if (lsm->opts.is_unique)
		key = lsm->key_def;
	else
		return;
	struct vy_tx *tx, *next_tx;
	rlist_foreach_entry_safe(tx, &xm->snapshot_active,
				 in_snapshot, next_tx) {
		if (tx->read_view->vlsn >= dump_lsn)
			break;
		if (tx->state != VINYL_TX_READY)
			continue;
		if (vy_tx_manager_check_active_tx(tx, lsm, key,
						  dump_generation))
			vy_tx_abort(tx);
	}
}

/**
 * Check sealed vy_mems of @a lsm for a concurrent write to the
 * same key as @a entry. Used by prepare-time conflict checks to
 * cover the case when vy_mem rotation happens during prepare:
 * the active mem is empty (just created) and the concurrent
 * write is in the just-sealed mem.
 */
static int
vy_tx_check_sealed_mems(struct vy_tx *tx, struct vy_lsm *lsm,
			struct vy_entry entry, struct key_def *key)
{
	if (vy_lsm_check_concurrent_write_mem(lsm, tx, entry, key)) {
		tx->xm->stat.conflict++;
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}
	return 0;
}

/**
 * Check for unique SK conflicts at prepare time. Called BEFORE
 * vy_lsm_set so the TX's own entry is not yet in the vy_mem.
 * Checks both the active mem and sealed mems.
 */
static int
vy_tx_check_unique_secondary(struct vy_tx *tx, struct txv *v,
			     struct key_def *key_def)
{
	/* NULL unique SK keys never conflict. */
	int multikey_idx = v->lsm->cmp_def->is_multikey ?
			   (int)v->entry.hint : MULTIKEY_NONE;
	if (key_def->is_nullable &&
	    tuple_key_contains_null(v->entry.stmt, key_def, multikey_idx))
		return 0;
	if (vy_mem_check_concurrent_write(v->mem, tx, v->entry, key_def)) {
		tx->xm->stat.conflict++;
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}
	return vy_tx_check_sealed_mems(tx, v->lsm, v->entry, key_def);
}

/**
 * Check for PK conflicts at prepare time. Uses @a prev -- the
 * previous version of the same key returned by vy_mem_insert
 * (O(1)) for the active mem, and scans sealed mems for conflicts
 * that may have landed there due to rotation during prepare.
 */
static int
vy_tx_check_primary(struct vy_tx *tx, struct vy_lsm *lsm,
		    struct vy_entry entry, struct vy_entry prev)
{
	if (prev.stmt != NULL &&
	    vy_stmt_lsn(prev.stmt) >= tx->read_view->vlsn + 1) {
		tx->xm->stat.conflict++;
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}
	if (vy_tx_check_sealed_mems(tx, lsm, entry, lsm->cmp_def) != 0)
		return -1;
	return 0;
}

int
vy_tx_prepare(struct vy_tx *tx)
{
	struct vy_tx_manager *xm = tx->xm;

	if (tx->state == VINYL_TX_ABORT) {
		/* Conflict is already accounted - see vy_tx_abort(). */
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}

	assert(tx->state == VINYL_TX_READY);
	if (vy_tx_is_ro(tx)) {
		if (vy_tx_is_in_snapshot(tx)) {
			/*
			 * A read-only snapshot TX with a prepared
			 * read view (vlsn >= MAX_LSN) must wait
			 * for the prepared TX to commit. Otherwise
			 * it could return data that is later rolled
			 * back if the prepared TX fails WAL. Keep
			 * state READY so that cascading abort in
			 * vy_tx_rollback_after_prepare can abort us.
			 */
			while (tx->read_view->vlsn >= MAX_LSN &&
			       tx->state == VINYL_TX_READY)
				fiber_cond_wait(&xm->read_view_cond);
			if (tx->state == VINYL_TX_ABORT) {
				diag_set(ClientError,
					 ER_TRANSACTION_CONFLICT);
				return -1;
			}
		}
		tx->state = VINYL_TX_COMMIT;
		rlist_del_entry(tx, in_snapshot);
		return 0;
	}
	tx->state = VINYL_TX_COMMIT;
	/*
	 * Snapshot TXs get read views eagerly (at vy_tx_begin),
	 * so they are always in a read view by the time they
	 * prepare. SSI TXs start with the global read view
	 * and may be sent to a real one on conflict.
	 */
	assert(!vy_tx_is_in_read_view(tx) ||
	       tx->isolation == TXN_ISOLATION_SNAPSHOT);
	assert(tx->read_view == &xm->global_read_view ||
	       tx->isolation == TXN_ISOLATION_SNAPSHOT);
	tx->psn = ++xm->psn;

	/*
	 * Flush transactional changes to the LSM tree.
	 * Sic: the loop below must not yield after recovery.
	 */
	/* repsert - REPLACE/UPSERT */
	struct tuple *delete = NULL, *repsert = NULL;
	MAYBE_UNUSED uint32_t current_space_id = 0;
	bool check_ww = vy_tx_is_in_snapshot(tx);
	struct txv *v;
	stailq_foreach_entry(v, &tx->log, next_in_log) {
		struct vy_lsm *lsm = v->lsm;
		struct vy_entry prev;
		struct vy_entry *p_prev = NULL;
		struct key_def *uniq = NULL;
		if (lsm->index_id == 0) {
			/* The beginning of the new txn_stmt is met. */
			current_space_id = lsm->space_id;
			repsert = NULL;
			delete = NULL;
			if (check_ww)
				p_prev = &prev;
		} else if (lsm->opts.is_unique && check_ww) {
			uniq = lsm->key_def;
		}
		assert(lsm->space_id == current_space_id);

		enum iproto_type type = vy_stmt_type(v->entry.stmt);

		if (lsm->index_id > 0 && repsert == NULL &&
		    type != IPROTO_DELETE) {
			/*
			 * With deferred DELETEs enabled, a REPLACE that was
			 * overwritten in the primary index may lack a DELETE
			 * in a secondary index. We must skip such a REPLACE
			 * because, since it's skipped in the primary index,
			 * we wouldn't generate a DELETE for it on compaction.
			 */
			v->is_overwritten = true;
		}

		/* Do not save statements that was overwritten by the same tx */
		if (v->is_overwritten || v->is_nop)
			continue;

		/* Optimize out INSERT + DELETE for the same key. */
		if (v->is_first_insert && type == IPROTO_DELETE)
			continue;

		if (v->is_first_insert && type == IPROTO_REPLACE) {
			/*
			 * There is no committed statement for the
			 * given key or the last statement is DELETE
			 * so we can turn REPLACE into INSERT.
			 */
			type = IPROTO_INSERT;
			vy_stmt_set_type(v->entry.stmt, type);
			/*
			 * In case of INSERT, no statement was actually
			 * overwritten so no need to generate a deferred
			 * DELETE for secondary indexes.
			 */
			uint8_t flags = vy_stmt_flags(v->entry.stmt);
			if (flags & VY_STMT_DEFERRED_DELETE) {
				vy_stmt_set_flags(v->entry.stmt, flags &
						  ~VY_STMT_DEFERRED_DELETE);
			}
		}

		if (!v->is_first_insert && type == IPROTO_INSERT) {
			/*
			 * INSERT following REPLACE means nothing,
			 * turn it into REPLACE.
			 */
			type = IPROTO_REPLACE;
			vy_stmt_set_type(v->entry.stmt, type);
		}

		/*
		 * Rotate the active in-memory tree if necessary and pin it
		 * to make sure it is not dumped until the transaction is
		 * complete.
		 */
		if (vy_lsm_rotate_mem_if_required(lsm) != 0)
			return -1;
		vy_mem_pin(lsm->mem);
		v->mem = lsm->mem;

		if (lsm->index_id == 0 &&
		    vy_stmt_flags(v->entry.stmt) & VY_STMT_DEFERRED_DELETE &&
		    vy_tx_handle_deferred_delete(tx, v) != 0)
			return -1;

		/* In secondary indexes only REPLACE/DELETE can be written. */
		vy_stmt_set_lsn(v->entry.stmt, MAX_LSN + tx->psn);
		struct tuple **region_stmt =
			(type == IPROTO_DELETE) ? &delete : &repsert;
		/*
		 * Check unique SK conflicts before inserting
		 * into vy_mem, so we don't find our own entry.
		 */
		if (uniq != NULL &&
		    vy_tx_check_unique_secondary(tx, v, uniq) != 0)
			return -1;
		if (vy_lsm_set(lsm, v->mem, v->entry, region_stmt, p_prev) != 0)
			return -1;
		v->region_stmt = *region_stmt;
		if (p_prev != NULL &&
		    vy_tx_check_primary(tx, lsm, v->entry, prev) != 0)
			return -1;
		vy_tx_send_readers_to_read_view(tx, v);
	}
	rlist_del_entry(tx, in_snapshot);
	assert(rlist_empty(&tx->in_prepared));
	rlist_add_tail_entry(&xm->prepared, tx, in_prepared);
	return 0;
}

void
vy_tx_commit(struct vy_tx *tx, int64_t lsn)
{
	assert(tx->state == VINYL_TX_COMMIT);
	struct vy_tx_manager *xm = tx->xm;

	xm->stat.commit++;

	if (vy_tx_is_ro(tx))
		goto out;

	assert(!rlist_empty(&tx->in_prepared));
	rlist_del_entry(tx, in_prepared);

	assert(xm->lsn <= lsn);
	xm->lsn = lsn;

	/* Fix LSNs of the records and commit changes. */
	struct txv *v;
	stailq_foreach_entry(v, &tx->log, next_in_log) {
		if (v->region_stmt != NULL) {
			struct vy_entry entry;
			entry.stmt = v->region_stmt;
			entry.hint = v->entry.hint;
			vy_stmt_set_lsn(v->region_stmt, lsn);
			vy_lsm_commit_stmt(v->lsm, v->mem, entry);
		}
		if (v->mem != NULL)
			vy_mem_unpin(v->mem);
	}

	/*
	 * When a transaction is prepared, its statements are
	 * stamped with pseudo-LSNs (MAX_LSN + psn). The
	 * tx_exists path in vy_tx_manager_read_view creates
	 * a shared read view at this pseudo-LSN, linking it
	 * to the prepared TX. Now that the real LSN is known,
	 * convert the pseudo-LSN to the real one so the
	 * dependent reader sees committed data at the correct
	 * LSN.
	 *
	 * Only convert pseudo-LSN read views (vlsn >= MAX_LSN).
	 * Real-LSN read views must not be mutated as they may
	 * be shared by multiple TXs.
	 */
	if (tx->read_view != &xm->global_read_view &&
	    tx->read_view->vlsn >= MAX_LSN) {
		tx->read_view->vlsn = lsn;
		if (tx->read_view->refs > 1)
			fiber_cond_broadcast(&xm->read_view_cond);
	}
out:
	vy_tx_destroy(tx);
	mempool_free(&xm->tx_mempool, tx);
}

static void
vy_tx_rollback_after_prepare(struct vy_tx *tx)
{
	assert(tx->state == VINYL_TX_COMMIT);

	/*
	 * There are two reasons of rollback_after_prepare:
	 * 1) Fail in the middle of vy_tx_prepare call.
	 * 2) Cascading rollback after WAL fail.
	 *
	 * In the first case, the transaction isn't in the list of prepared
	 * transactions hence there's no assertion that the tx->in_prepared
	 * link must not be an empty list head (rlist_del is a no-op if the
	 * link is an empty head).
	 */
	if (!rlist_empty(&tx->in_prepared)) {
		/*
		 * WAL failure: the TX completed prepare but failed
		 * the WAL write. Abort all snapshot TXs that have a
		 * prepared read view (vlsn >= MAX_LSN) -- they may
		 * have seen data from this TX.
		 */
		rlist_del_entry(tx, in_prepared);
		struct vy_tx *dep, *next_dep;
		rlist_foreach_entry_safe(dep, &tx->xm->snapshot_active,
					 in_snapshot, next_dep) {
			if (dep->read_view->vlsn >= MAX_LSN)
				vy_tx_abort(dep);
		}
		/* Wake read-only TXs waiting in vy_tx_prepare. */
		fiber_cond_broadcast(&tx->xm->read_view_cond);
	}

	struct txv *v;
	stailq_foreach_entry(v, &tx->log, next_in_log) {
		if (v->region_stmt != NULL) {
			struct vy_entry entry;
			entry.stmt = v->region_stmt;
			entry.hint = v->entry.hint;
			vy_lsm_rollback_stmt(v->lsm, v->mem, entry);
			vy_tx_abort_readers(tx, v);
		}
		if (v->mem != NULL)
			vy_mem_unpin(v->mem);
	}
}

void
vy_tx_rollback(struct vy_tx *tx)
{
	struct vy_tx_manager *xm = tx->xm;

	xm->stat.rollback++;

	if (tx->state == VINYL_TX_COMMIT)
		vy_tx_rollback_after_prepare(tx);

	vy_tx_destroy(tx);
	mempool_free(&xm->tx_mempool, tx);
}

int
vy_tx_begin_statement(struct vy_tx *tx, struct space *space, void **savepoint)
{
	if (tx->state == VINYL_TX_READY && vy_tx_is_in_read_view(tx) &&
	    tx->isolation != TXN_ISOLATION_SNAPSHOT)
		vy_tx_abort(tx);
	if (tx->state == VINYL_TX_ABORT) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}
	assert(tx->state == VINYL_TX_READY);
	tx->last_stmt_space = space;
	/*
	 * When want to add to the writer list, can't rely on the log emptiness.
	 * During recovery it is empty always for the data stored both in runs
	 * and xlogs. Must check the list member explicitly.
	 */
	if (rlist_empty(&tx->in_writers)) {
		assert(stailq_empty(&tx->log));
		rlist_add_entry(&tx->xm->writers, tx, in_writers);
	}
	*savepoint = stailq_last(&tx->log);
	return 0;
}

void
vy_tx_rollback_statement(struct vy_tx *tx, void *svp)
{
	if (tx->state == VINYL_TX_ABORT ||
	    tx->state == VINYL_TX_COMMIT)
		return;

	assert(tx->state == VINYL_TX_READY);
	struct stailq_entry *last = svp;
	struct stailq tail;
	stailq_cut_tail(&tx->log, last, &tail);
	/* Rollback statements in LIFO order. */
	stailq_reverse(&tail);
	struct txv *v, *tmp;
	stailq_foreach_entry_safe(v, tmp, &tail, next_in_log) {
		write_set_remove(&tx->write_set, v);
		if (v->overwritten != NULL) {
			/* Restore overwritten statement. */
			write_set_insert(&tx->write_set, v->overwritten);
			v->overwritten->is_overwritten = false;
		}
		tx->write_set_version++;
		txv_delete(v);
	}
	if (stailq_empty(&tx->log))
		rlist_del_entry(tx, in_writers);
	tx->last_stmt_space = NULL;
}

void
vy_tx_track(struct vy_tx *tx, struct vy_lsm *lsm,
	    struct vy_entry left, bool left_belongs,
	    struct vy_entry right, bool right_belongs)
{
	if (vy_tx_is_in_read_view(tx)) {
		/* No point in tracking reads. */
		return;
	}
	/*
	 * Snapshot TXs get a read view on first read (lazy vlsn).
	 * By the time vy_tx_track is called, the read view must
	 * already be assigned via vy_tx_read_view().
	 */
	assert(tx->isolation != TXN_ISOLATION_SNAPSHOT);

	struct vy_read_interval *new_interval;
	new_interval = vy_read_interval_new(tx, lsm, left, left_belongs,
					    right, right_belongs);

	/*
	 * Search for intersections in the transaction read set.
	 */
	struct stailq merge;
	stailq_create(&merge);

	struct vy_tx_read_set_iterator it;
	vy_tx_read_set_isearch_le(&tx->read_set, new_interval, &it);

	struct vy_read_interval *interval;
	interval = vy_tx_read_set_inext(&it);
	if (interval != NULL && interval->lsm == lsm) {
		if (vy_read_interval_cmpr(interval, new_interval) >= 0) {
			/*
			 * There is an interval in the tree spanning
			 * the new interval. Nothing to do.
			 */
			vy_read_interval_delete(new_interval);
			return;
		}
		if (vy_read_interval_should_merge(interval, new_interval))
			stailq_add_tail_entry(&merge, interval, in_merge);
	}

	if (interval == NULL)
		vy_tx_read_set_isearch_gt(&tx->read_set, new_interval, &it);

	while ((interval = vy_tx_read_set_inext(&it)) != NULL &&
	       interval->lsm == lsm &&
	       vy_read_interval_should_merge(new_interval, interval))
		stailq_add_tail_entry(&merge, interval, in_merge);

	/*
	 * Merge intersecting intervals with the new interval and
	 * remove them from the transaction and LSM tree read sets.
	 */
	if (!stailq_empty(&merge)) {
		vy_read_interval_unacct(new_interval);
		interval = stailq_first_entry(&merge, struct vy_read_interval,
					      in_merge);
		if (vy_read_interval_cmpl(new_interval, interval) > 0) {
			tuple_ref(interval->left.stmt);
			tuple_unref(new_interval->left.stmt);
			new_interval->left = interval->left;
			new_interval->left_belongs = interval->left_belongs;
		}
		interval = stailq_last_entry(&merge, struct vy_read_interval,
					     in_merge);
		if (vy_read_interval_cmpr(new_interval, interval) < 0) {
			tuple_ref(interval->right.stmt);
			tuple_unref(new_interval->right.stmt);
			new_interval->right = interval->right;
			new_interval->right_belongs = interval->right_belongs;
		}
		struct vy_read_interval *next_interval;
		stailq_foreach_entry_safe(interval, next_interval, &merge,
					  in_merge) {
			vy_tx_read_set_remove(&tx->read_set, interval);
			vy_lsm_read_set_remove(&lsm->read_set, interval);
			vy_read_interval_delete(interval);
		}
		vy_read_interval_acct(new_interval);
	}

	vy_tx_read_set_insert(&tx->read_set, new_interval);
	vy_lsm_read_set_insert(&lsm->read_set, new_interval);
}

void
vy_tx_track_point(struct vy_tx *tx, struct vy_lsm *lsm, struct vy_entry entry)
{
	assert(vy_stmt_is_full_key(entry.stmt, lsm->cmp_def));

	if (vy_tx_is_in_read_view(tx)) {
		/* No point in tracking reads. */
		return;
	}

	struct txv *v = write_set_search_key(&tx->write_set, lsm, entry);
	if (v != NULL && vy_stmt_type(v->entry.stmt) != IPROTO_UPSERT) {
		/* Reading from own write set is serializable. */
		return;
	}

	vy_tx_track(tx, lsm, entry, true, entry, true);
}

/**
 * Add one statement entry to a transaction. We add one entry
 * for each index, and with multikey indexes it is possible there
 * are multiple entries of a single statement in a single index.
 */
static int
vy_tx_set_entry(struct vy_tx *tx, struct vy_lsm *lsm, struct vy_entry entry)
{
	assert(vy_stmt_type(entry.stmt) != 0);

	struct txv *old = write_set_search_key(&tx->write_set, lsm, entry);
	if (old != NULL && old->entry.stmt == entry.stmt) {
		/*
		 * The inserted statement is already indexed in the write set.
		 * This may happen only if this is a multikey index and the
		 * indexed array has duplicate entries. Inserting a duplicate
		 * into the write set is pointless. Moreover, it may break
		 * assumptions taken by the optimizations applied below, like
		 * REPLACE + DELETE = NOP. Let's skip it.
		 */
		assert(lsm->cmp_def->is_multikey);
		assert(!old->is_overwritten);
		assert(old->entry.hint != entry.hint);
		return 0;
	}

	/**
	 * A statement in write set must have and unique lsn
	 * in order to differ it from cachable statements in mem and run.
	 */
	vy_stmt_set_lsn(entry.stmt, INT64_MAX);
	struct vy_entry applied = vy_entry_none();

	/* Found a match of the previous action of this transaction */
	if (old != NULL && vy_stmt_type(entry.stmt) == IPROTO_UPSERT) {
		assert(lsm->index_id == 0);
		uint8_t old_type = vy_stmt_type(old->entry.stmt);
		assert(old_type == IPROTO_UPSERT ||
		       old_type == IPROTO_INSERT ||
		       old_type == IPROTO_REPLACE ||
		       old_type == IPROTO_DELETE);
		(void) old_type;

		applied = vy_entry_apply_upsert(entry, old->entry,
						lsm->cmp_def, true);
		lsm->stat.upsert.applied++;
		if (applied.stmt == NULL)
			return -1;
		entry = applied;
		assert(vy_stmt_type(entry.stmt) != 0);
		lsm->stat.upsert.squashed++;
	}

	/* Allocate a MVCC container. */
	struct txv *v = txv_new(tx, lsm, entry);
	if (applied.stmt != NULL)
		tuple_unref(applied.stmt);

	if (old != NULL) {
		/* Leave the old txv in TX log but remove it from write set */
		assert(tx->write_size >= tuple_size(old->entry.stmt));
		tx->write_size -= tuple_size(old->entry.stmt);
		write_set_remove(&tx->write_set, old);
		old->is_overwritten = true;
		v->is_first_insert = old->is_first_insert;
		/*
		 * Inherit VY_STMT_DEFERRED_DELETE flag from the older
		 * statement so as to generate a DELETE for the tuple
		 * overwritten by this transaction.
		 */
		if (vy_stmt_flags(old->entry.stmt) & VY_STMT_DEFERRED_DELETE) {
			uint8_t flags = vy_stmt_flags(entry.stmt);
			vy_stmt_set_flags(entry.stmt, flags |
					  VY_STMT_DEFERRED_DELETE);
		}
	}

	if (old == NULL && vy_stmt_type(entry.stmt) == IPROTO_INSERT)
		v->is_first_insert = true;

	if (lsm->index_id > 0 && old != NULL && !old->is_nop &&
	    !vy_lsm_is_being_constructed(lsm)) {
		/*
		 * In a secondary index write set, DELETE statement purges
		 * exactly one older statement so REPLACE + DELETE is no-op.
		 * Moreover, DELETE + REPLACE can be treated as no-op, too,
		 * because secondary indexes don't store full tuples hence
		 * all REPLACE statements for the same key are equivalent.
		 * Therefore we can zap DELETE + REPLACE as there must be
		 * an older REPLACE for the same key stored somewhere in the
		 * index data.
		 *
		 * Anyway, we do not apply this optimization if secondary
		 * index is currently being built. Otherwise, we may face
		 * the situation when we are handling pair of DELETE + REPLACE
		 * requests redirected by on_replace trigger by the key that
		 * hasn't been already inserted into secondary index.
		 * It results in updated tuple in PK (with bumped lsn), but
		 * still not inserted in secondary index (since optimization
		 * annihilates it; meanwhile it is skipped in
		 * vinyl_space_build_index() as featuring bumped lsn).
		 * Finally, we'll get missing tuple in secondary index after
		 * it is built.
		 */
		enum iproto_type type = vy_stmt_type(entry.stmt);
		enum iproto_type old_type = vy_stmt_type(old->entry.stmt);
		if ((type == IPROTO_DELETE) != (old_type == IPROTO_DELETE))
			v->is_nop = true;
	}

	v->overwritten = old;
	write_set_insert(&tx->write_set, v);
	tx->write_set_version++;
	tx->write_size += tuple_size(entry.stmt);
	stailq_add_tail_entry(&tx->log, v, next_in_log);
	/*
	 * For snapshot TXs, check for concurrent writes via
	 * point lookup in sealed mems and on disk. The check
	 * runs AFTER the entry is added to the write set so
	 * that the dump-completion WW check can find it if a
	 * dump completes during the yield.
	 */
	if (vy_tx_is_in_snapshot(tx) &&
	    (lsm->index_id == 0 || lsm->opts.is_unique)) {
		if (vy_lsm_check_concurrent_write(lsm, tx, entry) != 0)
			return -1;
	}
	return 0;
}

int
vy_tx_set(struct vy_tx *tx, struct vy_lsm *lsm, struct tuple *stmt)
{
	if (vy_tx_is_in_read_view(tx) &&
	    tx->isolation != TXN_ISOLATION_SNAPSHOT) {
		/*
		 * If a conflict occurs while the first DML statement in
		 * a transaction is waiting for a disk read to check key
		 * uniqueness, the transaction will not be aborted by
		 * vy_tx_send_to_read_view, because technically it is still
		 * read-only. So in addition to vy_tx_begin_statement we
		 * need to abort transactions sent to read view when we
		 * add a new statement to the write set.
		 */
		assert(vy_tx_is_ro(tx));
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}
	struct vy_entry entry;
	vy_stmt_foreach_entry(entry, stmt, lsm->cmp_def) {
		if (vy_tx_set_entry(tx, lsm, entry) != 0)
			return -1;
	}
	return 0;
}

void
vy_tx_manager_abort_writers_for_ddl(struct vy_tx_manager *xm,
				    struct space *space, bool *need_wal_sync)
{
	*need_wal_sync = false;
	if (space->index_count == 0)
		return; /* no indexes, no conflicts */
	struct vy_lsm *lsm = vy_lsm(space->index[0]);
	struct vy_tx *tx;
	rlist_foreach_entry(tx, &xm->writers, in_writers) {
		if (tx->state == VINYL_TX_ABORT)
			continue;
		if (tx->last_stmt_space != space &&
		    write_set_search_key(&tx->write_set, lsm,
					 lsm->env->empty_key) == NULL)
			continue;
		/*
		 * We can't abort prepared transactions as they have
		 * already reached WAL. The caller needs to sync WAL
		 * to make sure they are gone.
		 */
		if (tx->state == VINYL_TX_COMMIT)
			*need_wal_sync = true;
		else
			vy_tx_abort(tx);
	}
}

void
vy_tx_manager_abort_writers_for_ro(struct vy_tx_manager *xm)
{
	struct vy_tx *tx;
	rlist_foreach_entry(tx, &xm->writers, in_writers) {
		/* Applier ignores ro flag. */
		if (tx->state == VINYL_TX_READY && !tx->is_applier_session)
			vy_tx_abort(tx);
	}
}

void
vy_txw_iterator_open(struct vy_txw_iterator *itr,
		     struct vy_txw_iterator_stat *stat,
		     struct vy_tx *tx, struct vy_lsm *lsm,
		     enum iterator_type iterator_type, struct vy_entry key)
{
	itr->stat = stat;
	itr->tx = tx;
	itr->lsm = lsm;
	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->version = UINT32_MAX;
	itr->curr_txv = NULL;
	itr->search_started = false;
}

/**
 * Position the iterator to the first entry in the transaction
 * write set satisfying the search criteria and following the
 * given key (pass NULL to start iteration).
 */
static void
vy_txw_iterator_seek(struct vy_txw_iterator *itr, struct vy_entry last)
{
	itr->stat->lookup++;
	itr->version = itr->tx->write_set_version;
	itr->curr_txv = NULL;

	struct vy_entry key = itr->key;
	enum iterator_type iterator_type = itr->iterator_type;
	if (last.stmt != NULL) {
		key = last;
		iterator_type = iterator_direction(iterator_type) > 0 ?
				ITER_GT : ITER_LT;
	}

	struct vy_lsm *lsm = itr->lsm;
	struct write_set_key k = { lsm, key };
	struct txv *txv;
	if (!vy_stmt_is_empty_key(key.stmt)) {
		if (iterator_type == ITER_EQ)
			txv = write_set_search(&itr->tx->write_set, &k);
		else if (iterator_type == ITER_GE || iterator_type == ITER_GT)
			txv = write_set_nsearch(&itr->tx->write_set, &k);
		else
			txv = write_set_psearch(&itr->tx->write_set, &k);
		if (txv == NULL || txv->lsm != lsm)
			return;
		if (vy_entry_compare(key, txv->entry, lsm->cmp_def) == 0) {
			while (true) {
				struct txv *next;
				if (iterator_type == ITER_LE ||
				    iterator_type == ITER_GT)
					next = write_set_next(&itr->tx->write_set, txv);
				else
					next = write_set_prev(&itr->tx->write_set, txv);
				if (next == NULL || next->lsm != lsm)
					break;
				if (vy_entry_compare(key, next->entry,
						     lsm->cmp_def) != 0)
					break;
				txv = next;
			}
			if (iterator_type == ITER_GT)
				txv = write_set_next(&itr->tx->write_set, txv);
			else if (iterator_type == ITER_LT)
				txv = write_set_prev(&itr->tx->write_set, txv);
		}
	} else if (iterator_type == ITER_LE) {
		txv = write_set_nsearch(&itr->tx->write_set, &k);
	} else {
		assert(iterator_type == ITER_GE);
		txv = write_set_psearch(&itr->tx->write_set, &k);
	}
	if (txv == NULL || txv->lsm != lsm)
		return;
	if (itr->iterator_type == ITER_EQ && last.stmt != NULL &&
	    vy_entry_compare(itr->key, txv->entry, lsm->cmp_def) != 0)
		return;
	itr->curr_txv = txv;
}

NODISCARD int
vy_txw_iterator_next(struct vy_txw_iterator *itr,
		     struct vy_history *history)
{
	vy_history_cleanup(history);
	if (!itr->search_started) {
		itr->search_started = true;
		vy_txw_iterator_seek(itr, vy_entry_none());
		goto out;
	}
	assert(itr->version == itr->tx->write_set_version);
	if (itr->curr_txv == NULL)
		return 0;
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		itr->curr_txv = write_set_prev(&itr->tx->write_set, itr->curr_txv);
	else
		itr->curr_txv = write_set_next(&itr->tx->write_set, itr->curr_txv);
	if (itr->curr_txv != NULL && itr->curr_txv->lsm != itr->lsm)
		itr->curr_txv = NULL;
	if (itr->curr_txv != NULL && itr->iterator_type == ITER_EQ &&
	    vy_entry_compare(itr->key, itr->curr_txv->entry,
			     itr->lsm->cmp_def) != 0)
		itr->curr_txv = NULL;
out:
	if (itr->curr_txv != NULL) {
		vy_stmt_counter_acct_tuple(&itr->stat->get,
					   itr->curr_txv->entry.stmt);
		return vy_history_append_stmt(history, itr->curr_txv->entry);
	}
	return 0;
}

NODISCARD int
vy_txw_iterator_skip(struct vy_txw_iterator *itr, struct vy_entry last,
		     struct vy_history *history)
{
	assert(!itr->search_started ||
	       itr->version == itr->tx->write_set_version);

	/*
	 * Check if the iterator is already positioned
	 * at the statement following last.
	 */
	if (itr->search_started &&
	    (itr->curr_txv == NULL || last.stmt == NULL ||
	     iterator_direction(itr->iterator_type) *
	     vy_entry_compare(itr->curr_txv->entry, last,
			      itr->lsm->cmp_def) > 0))
		return 0;

	vy_history_cleanup(history);

	itr->search_started = true;
	vy_txw_iterator_seek(itr, last);

	if (itr->curr_txv != NULL) {
		vy_stmt_counter_acct_tuple(&itr->stat->get,
					   itr->curr_txv->entry.stmt);
		return vy_history_append_stmt(history, itr->curr_txv->entry);
	}
	return 0;
}

NODISCARD int
vy_txw_iterator_restore(struct vy_txw_iterator *itr, struct vy_entry last,
			struct vy_history *history)
{
	if (!itr->search_started || itr->version == itr->tx->write_set_version)
		return 0;

	vy_txw_iterator_seek(itr, last);

	vy_history_cleanup(history);
	if (itr->curr_txv != NULL) {
		vy_stmt_counter_acct_tuple(&itr->stat->get,
					   itr->curr_txv->entry.stmt);
		if (vy_history_append_stmt(history, itr->curr_txv->entry) != 0)
			return -1;
	}
	return 1;
}

/**
 * Close a txw iterator.
 */
void
vy_txw_iterator_close(struct vy_txw_iterator *itr)
{
	(void)itr; /* suppress warn if NDEBUG */
	TRASH(itr);
}
