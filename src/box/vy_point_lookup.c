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
#include "vy_point_lookup.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <small/region.h>
#include <small/rlist.h>

#include "fiber.h"

#include "vy_lsm.h"
#include "vy_stmt.h"
#include "vy_tx.h"
#include "vy_mem.h"
#include "vy_run.h"
#include "vy_cache.h"
#include "vy_history.h"

/**
 * Scan TX write set for given key.
 * Add one or no statement to the history list.
 *
 * This scan never reports staleness via history->is_stale, unlike the
 * mem and run scans. The write set holds the transaction's own
 * uncommitted writes, which are always visible to the transaction
 * itself, so nothing is ever skipped by the read view. There is also
 * at most one entry per key, and write set statements are uncommitted
 * (lsn == INT64_MAX) and never enter the tuple cache -- vy_cache_add()
 * drops them by that lsn.
 */
static int
vy_point_lookup_scan_txw(struct vy_lsm *lsm, struct vy_tx *tx,
			 struct vy_entry key, struct vy_history *history)
{
	if (tx == NULL)
		return 0;
	lsm->stat.txw.iterator.lookup++;
	struct txv *txv =
		write_set_search_key(&tx->write_set, lsm, key);
	assert(txv == NULL || txv->lsm == lsm);
	if (txv == NULL)
		return 0;
	vy_stmt_counter_acct_tuple(&lsm->stat.txw.iterator.get,
				   txv->entry.stmt);
	return vy_history_append_stmt(history, txv->entry);
}

/**
 * Scan LSM tree cache for given key.
 * Add one or no statement to the history list.
 */
static int
vy_point_lookup_scan_cache(struct vy_lsm *lsm, const struct vy_read_view **rv,
			   struct vy_entry key, struct vy_history *history)
{
	lsm->cache.stat.lookup++;
	struct vy_entry entry = vy_cache_get(&lsm->cache, key);

	/* Unconfirmed data never enters the cache: see vy_cache_insert(). */
	assert(entry.stmt == NULL || !vy_stmt_is_prepared(entry.stmt));
	/*
	 * A newer cached version invisible in the read view is skipped
	 * here without reporting staleness via history->is_stale, unlike
	 * the mem and run scans below. This is safe because the tuple
	 * cache is a read-through subset of the memory and disk levels:
	 * any committed version it holds is also present in a mem or a
	 * run, so the same version is skipped -- and flagged stale -- by
	 * the mem or run scan. The result is therefore marked stale and
	 * kept out of the cache regardless, so a stale read can't overwrite
	 * a fresher cached version. Flagging it here too would be redundant.
	 */
	if (entry.stmt == NULL || vy_stmt_lsn(entry.stmt) > (*rv)->vlsn)
		return 0;

	vy_stmt_counter_acct_tuple(&lsm->cache.stat.get, entry.stmt);
	return vy_history_append_stmt(history, entry);
}

/**
 * Scan one particular mem.
 * Add found statements to the history list up to terminal statement.
 */
static int
vy_point_lookup_scan_mem(struct vy_lsm *lsm, struct vy_mem *mem,
			 const struct vy_read_view **rv, bool is_prepared_ok,
			 struct vy_entry key, struct vy_history *history,
			 int64_t *min_skipped_plsn)
{
	struct vy_mem_iterator mem_itr;
	vy_mem_iterator_open(&mem_itr, &lsm->stat.memory.iterator,
			     mem, ITER_EQ, key, rv, is_prepared_ok);
	struct vy_history mem_history;
	vy_history_create(&mem_history, &lsm->env->history_node_pool);
	int rc = vy_mem_iterator_next(&mem_itr, &mem_history);
	vy_history_splice(history, &mem_history);
	*min_skipped_plsn = MIN(*min_skipped_plsn, mem_itr.min_skipped_plsn);
	history->is_stale = history->is_stale || mem_itr.is_stale;
	vy_mem_iterator_close(&mem_itr);
	return rc;

}

/**
 * Scan all mems that belongs to the LSM tree.
 * Add found statements to the history list up to terminal statement.
 */
static int
vy_point_lookup_scan_mems(struct vy_lsm *lsm, struct vy_tx *tx,
			  const struct vy_read_view **rv, bool is_prepared_ok,
			  struct vy_entry key, struct vy_history *history)
{
	assert(lsm->mem != NULL);
	int64_t min_skipped_plsn = INT64_MAX;
	if (vy_point_lookup_scan_mem(lsm, lsm->mem, rv, is_prepared_ok,
				     key, history, &min_skipped_plsn) != 0)
		return -1;
	struct vy_mem *mem;
	rlist_foreach_entry(mem, &lsm->sealed, in_mems) {
		if (vy_history_is_terminal(history))
			break;
		if (vy_point_lookup_scan_mem(lsm, mem, rv, is_prepared_ok,
					     key, history,
					     &min_skipped_plsn) != 0)
			return -1;
	}
	/*
	 * Switch to read view if we skipped a prepared statement.
	 */
	if (tx != NULL && min_skipped_plsn != INT64_MAX) {
		vy_tx_send_to_read_view(tx, min_skipped_plsn);
		if (tx->state == VINYL_TX_ABORT) {
			diag_set(ClientError, ER_TRANSACTION_CONFLICT);
			return -1;
		}
	}
	return 0;
}

/**
 * Scan one particular slice.
 * Add found statements to the history list up to terminal statement.
 */
static int
vy_point_lookup_scan_slice(struct vy_lsm *lsm, struct vy_slice *slice,
			   const struct vy_read_view **rv, struct vy_entry key,
			   struct vy_history *history)
{
	/*
	 * The format of the statement must be exactly the space
	 * format with the same identifier to fully match the
	 * format in vy_mem.
	 */
	struct vy_run_iterator run_itr;
	vy_run_iterator_open(&run_itr, &lsm->stat.disk.iterator, slice,
			     ITER_EQ, key, rv, lsm->cmp_def, lsm->key_def,
			     lsm->disk_format);
	struct vy_history slice_history;
	vy_history_create(&slice_history, &lsm->env->history_node_pool);
	int rc = vy_run_iterator_next(&run_itr, &slice_history);
	vy_history_splice(history, &slice_history);
	history->is_stale = history->is_stale || run_itr.is_stale;
	vy_run_iterator_close(&run_itr);
	return rc;
}

/**
 * Find a range and scan all slices that belongs to the range.
 * Add found statements to the history list up to terminal statement.
 * All slices are pinned before first slice scan, so it's guaranteed
 * that complete history from runs will be extracted.
 */
static int
vy_point_lookup_scan_slices(struct vy_lsm *lsm, const struct vy_read_view **rv,
			    struct vy_entry key, struct vy_history *history)
{
	struct vy_range *range = vy_range_tree_find_by_key(&lsm->range_tree,
							   key);
	lsm->last_range = range;
	int slice_count = range->slice_count;
	size_t region_svp = region_used(&fiber()->gc);
	if (slice_count == 0)
		return 0;
	struct vy_slice **slices =
		xregion_alloc_array(&fiber()->gc, typeof(slices[0]),
				    slice_count);
	int i = 0;
	struct vy_slice *slice;
	rlist_foreach_entry(slice, &range->slices, in_range) {
		vy_slice_pin(slice);
		slices[i++] = slice;
	}
	assert(i == slice_count);
	ERROR_INJECT_YIELD(ERRINJ_VY_POINT_LOOKUP_DELAY);
	int rc = 0;
	for (i = 0; i < slice_count; i++) {
		if (rc == 0 && !vy_history_is_terminal(history))
			rc = vy_point_lookup_scan_slice(lsm, slices[i],
							rv, key, history);
		vy_slice_unpin(slices[i]);
	}
	region_truncate(&fiber()->gc, region_svp);
	return rc;
}

int
vy_point_lookup(struct vy_lsm *lsm, struct vy_tx *tx,
		const struct vy_read_view **rv,
		struct vy_entry key, bool keep_delete,
		struct vy_entry *ret)
{
	/* All key parts must be set for a point lookup. */
	assert(vy_stmt_is_full_key(key.stmt, lsm->cmp_def));
	assert(tx == NULL || tx->state == VINYL_TX_READY);

	*ret = vy_entry_none();
	int rc = 0;
	int64_t disk_bytes = 0;

	/* History list */
	struct vy_history history, mem_history, disk_history;
	vy_history_create(&history, &lsm->env->history_node_pool);
	vy_history_create(&mem_history, &lsm->env->history_node_pool);
	vy_history_create(&disk_history, &lsm->env->history_node_pool);

	rc = vy_point_lookup_scan_txw(lsm, tx, key, &history);
	if (rc != 0 || vy_history_is_terminal(&history))
		goto done;

	bool is_prepared_ok = tx != NULL ? vy_tx_is_prepared_ok(tx) : false;
	rc = vy_point_lookup_scan_cache(lsm, rv, key, &history);
	if (rc != 0 || vy_history_is_terminal(&history))
		goto done;

restart:
	rc = vy_point_lookup_scan_mems(lsm, tx, rv, is_prepared_ok,
				       key, &mem_history);
	if (rc != 0 || vy_history_is_terminal(&mem_history))
		goto done;

	/* Save version before yield */
	uint32_t mem_version = lsm->mem->version;
	uint32_t mem_list_version = lsm->mem_list_version;

	rc = vy_point_lookup_scan_slices(lsm, rv, key, &disk_history);
	if (rc != 0)
		goto done;

	if (tx != NULL && tx->state == VINYL_TX_ABORT) {
		/*
		 * The transaction was aborted while we were reading
		 * disk. We must stop now and return an error as this
		 * function could be called by a DML request aborted
		 * by a DDL operation: failing early will prevent it
		 * from dereferencing a destroyed space.
		 */
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		rc = -1;
		goto done;
	}

	if (mem_list_version != lsm->mem_list_version) {
		/*
		 * Mem list was changed during yield. This could be rotation
		 * or a dump. In case of dump the memory referenced by
		 * statement history is gone and we need to reread new history.
		 * This in unnecessary in case of rotation but since we
		 * cannot distinguish these two cases we always restart.
		 */
		vy_history_cleanup(&mem_history);
		vy_history_cleanup(&disk_history);
		goto restart;
	}

	if (mem_version != lsm->mem->version) {
		/*
		 * Rescan the memory level if its version changed while we
		 * were reading disk, because there may be new statements
		 * matching the search key.
		 */
		vy_history_cleanup(&mem_history);
		rc = vy_point_lookup_scan_mems(lsm, tx, rv, is_prepared_ok,
					       key, &mem_history);
		if (rc != 0)
			goto done;
		if (vy_history_is_terminal(&mem_history))
			vy_history_cleanup(&disk_history);
	}

	/*
	 * Before the splice consumes disk_history, sum the
	 * tuple sizes for read-amp tracking.  This is the same
	 * metric as range scans (tuple_size per disk statement):
	 * it captures redundancy (tombstones, shadowed versions)
	 * without conflating it with the inherent page-read cost
	 * that compaction cannot eliminate.
	 */
	struct vy_history_node *node;
	rlist_foreach_entry(node, &disk_history.stmts, link)
		disk_bytes += tuple_size(node->entry.stmt);
done:
	vy_history_splice(&history, &mem_history);
	vy_history_splice(&history, &disk_history);

	if (rc == 0) {
		int upserts_applied;
		rc = vy_history_apply(&history, lsm->cmp_def,
				      keep_delete, &upserts_applied, ret);
		lsm->stat.upsert.applied += upserts_applied;
		vy_lsm_acct_read_amp(lsm, lsm->last_range,
				     disk_bytes, ret->stmt);
		if (ret->stmt != NULL && history.is_stale)
			vy_stmt_add_flag(ret->stmt, VY_STMT_STALE);
	}
	vy_history_cleanup(&history);

	if (rc != 0)
		return -1;

	return 0;
}

int
vy_point_lookup_mem(struct vy_lsm *lsm, const struct vy_read_view **rv,
		    struct vy_entry key, struct vy_entry *ret)
{
	assert(vy_stmt_is_full_key(key.stmt, lsm->cmp_def));

	int rc;
	struct vy_history history;
	vy_history_create(&history, &lsm->env->history_node_pool);

	rc = vy_point_lookup_scan_cache(lsm, rv, key, &history);
	if (rc != 0 || vy_history_is_terminal(&history))
		goto done;

	rc = vy_point_lookup_scan_mems(lsm, /*tx=*/NULL, rv,
				       /*is_prepared_ok=*/true, key, &history);
	if (rc != 0 || vy_history_is_terminal(&history))
		goto done;

	*ret = vy_entry_none();
	goto out;
done:
	if (rc == 0) {
		int upserts_applied;
		rc = vy_history_apply(&history, lsm->cmp_def,
				      true, &upserts_applied, ret);
		lsm->stat.upsert.applied += upserts_applied;
	}
out:
	vy_history_cleanup(&history);
	return rc;
}

/**
 * Check whether @a history holds a version with LSN >= @a min_lsn.
 *
 * @param history Statement history to inspect.
 * @param min_lsn Lowest LSN considered a conflict.
 *
 * @retval true  The newest history entry has LSN >= min_lsn.
 * @retval false History is empty or its newest entry is older.
 */
static inline bool
vy_history_has_lsn(struct vy_history *history, int64_t min_lsn)
{
	if (rlist_empty(&history->stmts))
		return false;
	struct vy_history_node *node = rlist_first_entry(
		&history->stmts, struct vy_history_node, link);
	return vy_stmt_lsn(node->entry.stmt) >= min_lsn;
}

int
vy_lsm_check_concurrent_write(struct vy_lsm *lsm, struct vy_tx *tx,
			      struct vy_entry entry)
{
	assert(lsm->index_id == 0 || lsm->opts.is_unique);
	ERROR_INJECT(ERRINJ_VY_CHECK_CONCURRENT_WRITE_DELAY,
		     fiber_sleep(0));
	ERROR_INJECT_GATE(ERRINJ_VY_CHECK_CONCURRENT_WRITE_GATE);
	struct key_def *key_def = lsm->index_id > 0 ?
				  lsm->key_def : lsm->cmp_def;
	int multikey_idx = lsm->cmp_def->is_multikey ?
			   (int)entry.hint : MULTIKEY_NONE;
	/* NULL unique SK keys never conflict. */
	if (lsm->index_id > 0 && key_def->is_nullable &&
	    tuple_key_contains_null(entry.stmt, key_def, multikey_idx))
		return 0;
	int64_t min_lsn = tx->read_view->vlsn + 1;
	bool found = false;
	int rc = 0;
	/*
	 * Scan sealed mems. The active vy_mem is checked at
	 * prepare time via the BPS tree prev entry, and at
	 * dump completion via vy_tx_manager_check_concurrent_write.
	 */
	if (vy_lsm_check_concurrent_write_mem(lsm, tx, entry, key_def)) {
		found = true;
		goto done;
	}
	/*
	 * lsm->dump_lsn is the max committed LSN on disk,
	 * including entries from sealed mems that were dumped
	 * and freed. If it is below the TX's read view, all
	 * entries on disk are visible to the TX. dump_lsn is
	 * -1 when no dumps have happened yet, which is always
	 * less than min_lsn, so this also covers the no-dumps
	 * case.
	 */
	if (lsm->dump_lsn < min_lsn)
		goto done;
	/*
	 * For unique SK, extract the SK-only key for cache
	 * and disk scan. The partial key positions iterators
	 * at the start of the SK group.
	 */
	struct vy_entry key = entry;
	struct tuple *sk_key = NULL;
	if (lsm->index_id > 0) {
		sk_key = vy_stmt_extract_key(entry.stmt, key_def,
					     lsm->env->key_format,
					     multikey_idx);
		if (sk_key == NULL)
			return -1;
		key.stmt = sk_key;
		key.hint = vy_stmt_hint(sk_key, lsm->cmp_def);
	}
	const struct vy_read_view **rv = &tx->xm->p_global_read_view;

	/* Check the tuple cache. */
	struct vy_history history;
	vy_history_create(&history, &lsm->env->history_node_pool);
	rc = vy_point_lookup_scan_cache(lsm, rv, key, &history);
	if (rc != 0)
		goto done_cleanup;
	if (!rlist_empty(&history.stmts)) {
		found = vy_history_has_lsn(&history, min_lsn);
		goto done_cleanup;
	}

	/*
	 * Scan disk slices. Slices are ordered newest-first.
	 * Stop once we hit one with max_lsn < min_lsn (all
	 * subsequent are older). Pin the LSM to prevent
	 * use-after-free if the space is dropped during a
	 * yield.
	 *
	 * For unique SK, a single slice may contain multiple
	 * entries with the same SK but different PKs (from
	 * insert/delete/insert cycles). Iterate through all
	 * matching key_def keys using the run iterator.
	 */
	vy_lsm_ref(lsm);
	struct vy_range *range = vy_range_tree_find_by_key(
		&lsm->range_tree, key);
	if (range->slice_count == 0)
		goto done_cleanup_unref;
	size_t region_svp = region_used(&fiber()->gc);
	struct vy_slice **slices = xregion_alloc_array(
		&fiber()->gc, typeof(slices[0]), range->slice_count);
	int count = 0;
	struct vy_slice *slice;
	rlist_foreach_entry(slice, &range->slices, in_range) {
		if (slice->run->info.max_lsn < min_lsn)
			break;
		vy_slice_pin(slice);
		slices[count++] = slice;
	}
	for (int i = 0; i < count; i++) {
		if (rc == 0 && !found) {
			struct vy_run_iterator run_itr;
			vy_run_iterator_open(
				&run_itr,
				&lsm->stat.disk.iterator,
				slices[i], ITER_EQ, key, rv,
				lsm->cmp_def, lsm->key_def,
				lsm->disk_format);
			do {
				vy_history_cleanup(&history);
				rc = vy_run_iterator_next(&run_itr,
							  &history);
				ERROR_INJECT_YIELD(
					ERRINJ_VY_POINT_LOOKUP_LSN_DELAY);
				if (rc != 0)
					break;
				if (rlist_empty(&history.stmts))
					break;
				found = vy_history_has_lsn(
					&history, min_lsn);
			} while (!found);
			vy_run_iterator_close(&run_itr);
			if (tx->state == VINYL_TX_ABORT) {
				diag_set(ClientError,
					 ER_TRANSACTION_CONFLICT);
				rc = -1;
			}
		}
		vy_slice_unpin(slices[i]);
	}
	region_truncate(&fiber()->gc, region_svp);
done_cleanup_unref:
	vy_lsm_unref(lsm);
done_cleanup:
	vy_history_cleanup(&history);
	if (sk_key != NULL)
		tuple_unref(sk_key);
done:
	if (rc == 0 && found) {
		tx->xm->stat.conflict++;
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		rc = -1;
	}
	return rc;
}

/** See the comment in vy_point_lookup.h. */
int
vy_get_by_secondary_tuple(struct vy_lsm *lsm, struct vy_tx *tx,
			  const struct vy_read_view **rv,
			  struct vy_entry entry, struct vy_entry *result,
			  int64_t *delete_lsn)
{
	int rc = 0;
	assert(lsm->index_id > 0);

	/*
	 * A full tuple that is not maybe stale is the primary
	 * cache's resident object: the newest committed version
	 * of its row, found under the row's own key. Serve it
	 * without the resolving lookup. The read is still
	 * tracked against the primary index: a DELETE of the
	 * row may be deferred -- written to the primary alone --
	 * and the secondary read set would never see it.
	 */
	if (!vy_stmt_is_key(entry.stmt) &&
	    !vy_stmt_maybe_stale(entry.stmt, /*is_primary=*/false)) {
		if (tx != NULL) {
			struct vy_entry pk_entry;
			pk_entry.stmt = entry.stmt;
			pk_entry.hint = vy_stmt_hint(entry.stmt,
						     lsm->pk->cmp_def);
			vy_tx_track_point(tx, lsm->pk, pk_entry);
		}
		tuple_ref(entry.stmt);
		*result = entry;
		return 0;
	}

	/*
	 * Lookup the full tuple by a secondary statement.
	 * There are two cases: the secondary statement may be
	 * a key, if we got this tuple from disk, in which case
	 * we need to extract the primary key parts from it; or
	 * it may be a full tuple, if we got this tuple from
	 * the tuple cache or level 0, in which case we may pass
	 * it immediately to the iterator.
	 */
	struct vy_entry key;
	if (vy_stmt_is_key(entry.stmt)) {
		key.stmt = vy_stmt_extract_key(entry.stmt, lsm->pk_in_cmp_def,
					       lsm->env->key_format,
					       MULTIKEY_NONE);
		if (key.stmt == NULL)
			return -1;
	} else {
		key.stmt = entry.stmt;
		tuple_ref(key.stmt);
	}
	key.hint = vy_stmt_hint(key.stmt, lsm->pk->cmp_def);

	lsm->pk->stat.lookup++;

	struct vy_entry pk_entry;
	if (vy_point_lookup(lsm->pk, tx, rv, key, /*keep_delete=*/true,
			    &pk_entry) != 0) {
		rc = -1;
		goto out;
	}

	bool match = false;
	struct vy_entry full_entry;
	if (pk_entry.stmt != NULL &&
	    vy_stmt_type(pk_entry.stmt) != IPROTO_DELETE) {
		vy_stmt_foreach_entry(full_entry, pk_entry.stmt, lsm->cmp_def) {
			if (vy_entry_compare(full_entry, entry,
					     lsm->cmp_def) == 0) {
				match = true;
				break;
			}
		}
	}
	if (!match) {
		/*
		 * If the primary index statement deleting the target
		 * key from the secondary index hasn't been confirmed,
		 * we must track it in the transaction read set because
		 * it may still be rolled back due to a WAL error, in which
		 * case the overwritten tuple matching the secondary index
		 * key can be reinstantiated.
		 */
		if (tx != NULL && pk_entry.stmt != NULL &&
		    vy_stmt_is_prepared(pk_entry.stmt))
			vy_tx_track_point(tx, lsm->pk, pk_entry);
		/*
		 * If a tuple read from a secondary index doesn't
		 * match the tuple corresponding to it in the
		 * primary index, it must have been overwritten or
		 * deleted, but the DELETE statement hasn't been
		 * propagated to the secondary index yet. In this
		 * case silently skip this tuple.
		 */
		/*
		 * Charge the wasted primary lookup to the primary's
		 * read-amp stat.  The orphan exists because the
		 * primary hasn't compacted to produce deferred
		 * deletes yet.  Use the primary tuple size to reflect
		 * the actual cost of the unnecessary lookup.  This
		 * signal accumulates across all secondary indexes,
		 * eventually triggering primary compaction to clean
		 * up deferred deletes.
		 */
		if (pk_entry.stmt != NULL)
			vy_lsm_acct_read_amp(lsm->pk,
					     lsm->pk->last_range,
					     tuple_size(pk_entry.stmt),
					     NULL);
		vy_stmt_counter_acct_tuple(&lsm->stat.skip, entry.stmt);
		if (pk_entry.stmt != NULL) {
			vy_stmt_counter_acct_tuple(&lsm->pk->stat.skip,
						   pk_entry.stmt);
			*delete_lsn = vy_stmt_lsn(pk_entry.stmt);
			tuple_unref(pk_entry.stmt);
		}
		*result = vy_entry_none();
		goto out;
	}

	/*
	 * Even though the tuple is tracked in the secondary index
	 * read set, we still must track the full tuple read from
	 * the primary index, otherwise the transaction won't be
	 * aborted if this tuple is overwritten or deleted, because
	 * the DELETE statement is not written to secondary indexes
	 * immediately.
	 */
	if (tx != NULL)
		vy_tx_track_point(tx, lsm->pk, pk_entry);

	vy_cache_add(&lsm->pk->cache, pk_entry);

	vy_stmt_counter_acct_tuple(&lsm->pk->stat.get, pk_entry.stmt);
	*result = full_entry;
out:
	tuple_unref(key.stmt);
	return rc;
}
