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
#include "vy_lsm.h"

#include "trivia/util.h"
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <small/mempool.h>

#include "assoc.h"
#include "diag.h"
#include "fiber.h"
#include "errcode.h"
#include "histogram.h"
#include "index_def.h"
#include "say.h"
#include "schema.h"
#include "tuple.h"
#include "trigger.h"
#include "vy_log.h"
#include "vy_mem.h"
#include "vy_range.h"
#include "vy_run.h"
#include "vy_stat.h"
#include "vy_tx.h"
#include "vy_stmt.h"
#include "vy_upsert.h"
#include "vy_history.h"
#include "vy_read_set.h"

/*
 * It doesn't make much sense to create too small ranges as this
 * would make the overhead associated with file creation prominent
 * and increase the number of open files. So we never create ranges
 * less than 128 MB.
 */
static const int64_t VY_MIN_RANGE_SIZE = 128 * 1024 * 1024;

/**
 * We want a single compaction job to finish in reasonable time
 * so we limit the range size to 2 GB.
 */
static const int64_t VY_MAX_RANGE_SIZE = 2LL * 1024 * 1024 * 1024;

/**
 * Probe the last-level bloom filter on every Nth blind write
 * to estimate the overwrite rate. Low enough to be negligible
 * on the hot path, high enough to give statistically useful
 * samples quickly (~100 samples after 1600 writes).
 *
 * The first sample-rate worth of blind writes after a counter
 * reset is sampled unconditionally, so the estimate starts
 * converging immediately instead of defaulting to "all blind
 * writes are new" for the first sample period.
 */
enum { VY_BLIND_WRITE_SAMPLE_RATE = 16 };

int
vy_lsm_env_create(struct vy_lsm_env *env, const char *path,
		  int64_t *p_generation, struct tuple_format *key_format,
		  vy_upsert_thresh_cb upsert_thresh_cb,
		  void *upsert_thresh_arg,
		  vy_compaction_trigger_cb compaction_trigger_cb,
		  void *compaction_trigger_arg)
{
	/*
	 * The bounds of the whole key space: the empty key marked
	 * infimum sorts before every key, marked supremum after
	 * every key. Range bounds are always bound keys, and the
	 * leftmost and rightmost ranges use these singletons.
	 * Full scans use them as search keys.
	 */
	env->key_inf.hint = HINT_NONE;
	env->key_inf.stmt = vy_key_new(key_format, NULL, 0);
	env->key_sup.hint = HINT_NONE;
	env->key_sup.stmt = vy_key_new(key_format, NULL, 0);
	if (env->key_inf.stmt == NULL || env->key_sup.stmt == NULL) {
		if (env->key_inf.stmt != NULL)
			tuple_unref(env->key_inf.stmt);
		if (env->key_sup.stmt != NULL)
			tuple_unref(env->key_sup.stmt);
		return -1;
	}
	vy_stmt_add_flag(env->key_inf.stmt, VY_STMT_INFIMUM);
	vy_stmt_add_flag(env->key_sup.stmt, VY_STMT_SUPREMUM);
	env->path = path;
	env->p_generation = p_generation;
	env->key_format = key_format;
	tuple_format_ref(key_format);
	env->upsert_thresh_cb = upsert_thresh_cb;
	env->upsert_thresh_arg = upsert_thresh_arg;
	env->compaction_trigger_cb = compaction_trigger_cb;
	env->compaction_trigger_arg = compaction_trigger_arg;
	env->too_long_threshold = TIMEOUT_INFINITY;
	env->lsm_count = 0;
	memset(&env->dict_stat, 0, sizeof(env->dict_stat));
	mempool_create(&env->history_node_pool, cord_slab_cache(),
		       sizeof(struct vy_history_node));
	return 0;
}

void
vy_lsm_env_destroy(struct vy_lsm_env *env)
{
	tuple_unref(env->key_inf.stmt);
	tuple_unref(env->key_sup.stmt);
	tuple_format_unref(env->key_format);
	mempool_destroy(&env->history_node_pool);
}

struct vy_dict *
vy_lsm_lookup_dict(struct vy_lsm *lsm, int64_t id)
{
	struct mh_i64ptr_t *h = lsm->dict_hash;
	mh_int_t k = mh_i64ptr_find(h, id, NULL);
	if (k == mh_end(h))
		return NULL;
	return mh_i64ptr_node(h, k)->val;
}

/**
 * Callback invoked by vy_dict_unref() just before the last
 * reference is dropped. Removes the dict from the LSM tree's
 * hash and updates stats. The dict itself is freed by
 * vy_dict_unref().
 */
static void
vy_dict_on_drop(struct vy_dict *dict, void *ctx)
{
	struct vy_lsm *lsm = ctx;
	assert(dict->id != 0);
	struct mh_i64ptr_t *h = lsm->dict_hash;
	mh_int_t k = mh_i64ptr_find(h, dict->id, NULL);
	if (k != mh_end(h))
		mh_i64ptr_del(h, k, NULL);
	vy_dict_stat_on_drop(&lsm->env->dict_stat, dict->size);
}

void
vy_lsm_add_dict(struct vy_lsm *lsm, struct vy_dict *dict)
{
	struct mh_i64ptr_t *h = lsm->dict_hash;
	struct mh_i64ptr_node_t node = { dict->id, dict };
	struct mh_i64ptr_node_t *old_node = &(typeof(*old_node)) {};
	mh_i64ptr_put(h, &node, &old_node, NULL);
	assert(old_node == NULL);
	dict->on_drop = vy_dict_on_drop;
	dict->on_drop_ctx = lsm;
	vy_dict_stat_on_create(&lsm->env->dict_stat, dict->size);
}

void
vy_lsm_apply_dict_cfg(struct vy_lsm *lsm)
{
	struct vy_dict_last *last = &lsm->dict_last;
	if (!lsm->opts.compression_dict) {
		/* Compression disabled: forget dict. */
		last->train_period = 0;
		if (last->dict != NULL) {
			vy_dict_unref(last->dict);
			last->dict = NULL;
		}
	} else if (last->train_period <= 0) {
		/* Compression just enabled: start training. */
		last->train_period = VY_DICT_TRAIN_PERIOD_DEFAULT;
	}
}

void
vy_lsm_accept_dict_sample(struct vy_lsm *lsm, struct vy_dict_sample *sample)
{
	vy_dict_last_adjust_period(&lsm->dict_last, sample);
	/*
	 * Dict compression may have been disabled via alter()
	 * while the task was in flight. Don't install a new dict.
	 */
	if (!lsm->opts.compression_dict)
		return;
	struct vy_dict *old = lsm->dict_last.dict;
	/*
	 * Safety net: if the active dict hasn't been committed to
	 * vylog yet (id == 0), a previous training result is still
	 * pending.  Don't touch it.  In practice this cannot happen
	 * because vy_run_prepare() always commits the pending dict
	 * before the next task runs (see comment there).
	 */
	if ((old == NULL || old->id != 0) &&
	    /* sample->size is set if we have an improved new dict */
	    (sample->size > 0 ||
	     /*
	      * The old dictionary should sometimes be dropped
	      * even if no new dictionary has been trained.
	      * It's when the new dictionary (trained on the
	      * current data) cannot beat plain ZSTD by at least
	      * VY_DICT_MIN_GAIN_PCT. Then the old dictionary
	      * -- trained on older data -- is certainly not helping
	      * either.
	      */
	     (sample->stat_delta.train_rejected > 0 &&
	      sample->base_dict != NULL &&
	      sample->index_stat.dict_gain * 100 <
	      VY_DICT_MIN_GAIN_PCT))) {
		lsm->stat.dict = sample->index_stat;

		if (old) {
			vy_dict_unref(old);
			lsm->dict_last.dict = NULL;
			vy_dict_last_reset_period(&lsm->dict_last);
		}

		if (sample->size > 0) {
			/* The training was successful. */
			struct vy_dict *new_dict;
			new_dict = vy_dict_new(0, sample->data, sample->size,
					       lsm->opts.compression_level);
			if (new_dict == NULL)
				return; /* no memory - no dict */
			vy_dict_ref(new_dict);
			lsm->dict_last.dict = new_dict;
		}
	}
}

const char *
vy_lsm_name(struct vy_lsm *lsm)
{
	char *buf = tt_static_buf();
	snprintf(buf, TT_STATIC_BUF_LEN, "%u/%u",
		 (unsigned)lsm->space_id, (unsigned)lsm->index_id);
	return buf;
}

size_t
vy_lsm_mem_tree_size(struct vy_lsm *lsm)
{
	struct vy_mem *mem;
	size_t size = vy_mem_tree_mem_used(&lsm->mem->tree);
	rlist_foreach_entry(mem, &lsm->sealed, in_mems)
		size += vy_mem_tree_mem_used(&mem->tree);
	return size;
}

/**
 * Sum per-type statement stats across the active mem and all
 * sealed mems. Only meaningful on the primary index.
 */
struct vy_stmt_stat
vy_lsm_mem_stmt_stat(struct vy_lsm *lsm)
{
	assert(lsm->index_id == 0);
	assert(lsm->mem->stmt != NULL);
	struct vy_stmt_stat s = *lsm->mem->stmt;
	struct vy_mem *mem;
	rlist_foreach_entry(mem, &lsm->sealed, in_mems) {
		assert(mem->stmt != NULL);
		vy_stmt_stat_add(&s, mem->stmt);
	}
	return s;
}

struct vy_lsm *
vy_lsm_new(struct vy_lsm_env *lsm_env, struct vy_cache_env *cache_env,
	   struct vy_mem_env *mem_env, struct index_def *index_def,
	   struct tuple_format *format, struct vy_lsm *pk, uint32_t group_id)
{
	static int64_t run_buckets[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 50, 100,
	};

	assert(index_def->key_def->part_count > 0);
	assert(index_def->iid == 0 || pk != NULL);

	struct vy_lsm *lsm = calloc(1, sizeof(struct vy_lsm));
	if (lsm == NULL) {
		diag_set(OutOfMemory, sizeof(struct vy_lsm),
			 "calloc", "struct vy_lsm");
		goto fail;
	}
	lsm->env = lsm_env;

	struct key_def *key_def = key_def_dup(index_def->key_def);
	struct key_def *cmp_def = key_def_dup(index_def->cmp_def);
	/*
	 * A unique nullable key definition extended with primary key parts
	 * (cmp_def) assumes that two tuples are equal *without* comparing
	 * primary key fields if all secondary key fields are equal and not
	 * nulls, see tuple_compare_slowpath(). This is a hack required to
	 * ignore the uniqueness constraint for nulls in memtx. The memtx
	 * engine can't use the secondary key definition as is (key_def) for
	 * comparing tuples in the index tree, as it does for a non-nullable
	 * unique index, because this wouldn't allow insertion of any
	 * duplicates, including nulls. It couldn't use cmp_def without this
	 * hack, either, because then conflicting tuples with the same
	 * secondary key fields would always compare as not equal due to
	 * different primary key parts.
	 *
	 * For Vinyl, this hack isn't required because it explicitly skips
	 * the uniqueness check if any of the indexed fields are nulls, see
	 * vy_check_is_unique_secondary(). Furthermore, this hack is harmful
	 * because Vinyl relies on the fact that two tuples compare as equal by
	 * cmp_def if and only if *all* key fields (both secondary and primary)
	 * are equal. For example, this is used in the transaction manager,
	 * which overwrites statements equal by cmp_def, see vy_tx_set_entry().
	 *
	 * We disable this hack by resetting unique_part_count in cmp_def.
	 */
	cmp_def->unique_part_count = cmp_def->part_count;

	lsm->cmp_def = cmp_def;
	lsm->key_def = key_def;
	if (index_def->iid == 0) {
		/*
		 * Disk tuples can be returned to an user from a
		 * primary key. And they must have field
		 * definitions as well as space->format tuples.
		 */
		lsm->disk_format = format;
	} else {
		/*
		 * To save disk space, we do not store full tuples
		 * in secondary index runs. Instead we only store
		 * extended keys (i.e. keys consisting of secondary
		 * and primary index parts). This is enough to look
		 * up a full tuple in the primary index.
		 */
		lsm->disk_format = lsm_env->key_format;

		lsm->pk_in_cmp_def = key_def_find_pk_in_cmp_def(lsm->cmp_def,
								pk->key_def,
								&fiber()->gc);
		if (lsm->pk_in_cmp_def == NULL)
			goto fail_pk_in_cmp_def;
	}
	tuple_format_ref(lsm->disk_format);

	if (vy_lsm_stat_create(&lsm->stat) != 0)
		goto fail_stat;

	lsm->run_hist = histogram_new(run_buckets, lengthof(run_buckets));
	if (lsm->run_hist == NULL)
		goto fail_run_hist;

	lsm->mem = vy_mem_new(mem_env, &lsm->stat.memory, cmp_def, format,
			      *lsm->env->p_generation,
			      space_cache_version,
			      index_def->iid);
	if (lsm->mem == NULL)
		goto fail_mem;

	lsm->id = -1;
	lsm->dump_lsn = -1;
	lsm->commit_lsn = -1;
	vy_cache_create(&lsm->cache, cache_env, cmp_def, index_def->iid == 0);
	rlist_create(&lsm->sealed);
	vy_range_tree_new(&lsm->range_tree);
	vy_range_heap_create(&lsm->range_heap);
	rlist_create(&lsm->runs);
	vy_dict_last_create(&lsm->dict_last, index_def->opts.compression_dict);
	lsm->dict_hash = mh_i64ptr_new();
	if (lsm->dict_hash == NULL) {
		diag_set(OutOfMemory, 0, "mh_i64ptr_new", "dict_hash");
		goto fail_dict_hash;
	}
	lsm->pk = pk;
	if (pk != NULL)
		vy_lsm_ref(pk);
	lsm->mem_format = format;
	tuple_format_ref(lsm->mem_format);
	heap_node_create(&lsm->in_dump);
	heap_node_create(&lsm->in_compaction);
	lsm->space_id = index_def->space_id;
	lsm->index_id = index_def->iid;
	lsm->group_id = group_id;
	lsm->opts = index_def->opts;
	vy_lsm_read_set_new(&lsm->read_set);
	rlist_create(&lsm->on_destroy);

	lsm_env->lsm_count++;
	return lsm;

fail_dict_hash:
	vy_mem_delete(lsm->mem);
fail_mem:
	histogram_delete(lsm->run_hist);
fail_run_hist:
	vy_lsm_stat_destroy(&lsm->stat);
fail_stat:
	tuple_format_unref(lsm->disk_format);
	if (lsm->pk_in_cmp_def != NULL)
		key_def_delete(lsm->pk_in_cmp_def);
fail_pk_in_cmp_def:
	key_def_delete(cmp_def);
	key_def_delete(key_def);
	free(lsm);
fail:
	return NULL;
}

static struct vy_range *
vy_range_tree_free_cb(vy_range_tree_t *t, struct vy_range *range, void *arg)
{
	(void)t;
	(void)arg;
	struct vy_slice *slice;
	rlist_foreach_entry(slice, &range->slices, in_range)
		assert(slice->pin_count == 0);
	vy_range_delete(range);
	return NULL;
}

void
vy_lsm_delete(struct vy_lsm *lsm)
{
	trigger_run(&lsm->on_destroy, lsm);

	assert(heap_node_is_stray(&lsm->in_dump));
	assert(heap_node_is_stray(&lsm->in_compaction));
	assert(vy_lsm_read_set_empty(&lsm->read_set));
	assert(lsm->env->lsm_count > 0);

	lsm->env->lsm_count--;
	lsm->env->compaction_queue_size -=
			lsm->stat.disk.compaction.queue.bytes;
	if (lsm->index_id == 0)
		lsm->env->compacted_data_size -=
			lsm->stat.disk.last_level_count.bytes;
	if (lsm->pk != NULL)
		vy_lsm_unref(lsm->pk);

	struct vy_mem *mem, *next_mem;
	rlist_foreach_entry_safe(mem, &lsm->sealed, in_mems, next_mem)
		vy_mem_delete(mem);
	vy_mem_delete(lsm->mem);

	vy_dict_last_destroy(&lsm->dict_last);

	struct vy_run *run, *next_run;
	rlist_foreach_entry_safe(run, &lsm->runs, in_lsm, next_run)
		vy_lsm_remove_run(lsm, run);

	vy_range_tree_iter(&lsm->range_tree, NULL, vy_range_tree_free_cb, NULL);
	/*
	 * Clean up any dicts still in the hash. Normally the hash
	 * is empty at this point because all runs have been unrefed
	 * (which unrefs their dicts). Orphaned dicts can remain
	 * after a failed recovery, when dicts were loaded from the
	 * log but no runs ended up referencing them.
	 *
	 * The hash does not hold a reference (that would create a
	 * ref cycle), so orphaned dicts have refs == 0. Remove
	 * them from the hash and free directly.
	 */
	mh_int_t k;
	while ((k = mh_first(lsm->dict_hash)) != mh_end(lsm->dict_hash)) {
		struct vy_dict *dict = mh_i64ptr_node(lsm->dict_hash, k)->val;
		assert(dict->refs == 0);
		mh_i64ptr_del(lsm->dict_hash, k, NULL);
		vy_dict_stat_on_drop(&lsm->env->dict_stat, dict->size);
		vy_dict_delete(dict);
	}
	mh_i64ptr_delete(lsm->dict_hash);
	vy_range_heap_destroy(&lsm->range_heap);
	tuple_format_unref(lsm->disk_format);
	key_def_delete(lsm->cmp_def);
	key_def_delete(lsm->key_def);
	if (lsm->pk_in_cmp_def != NULL)
		key_def_delete(lsm->pk_in_cmp_def);
	histogram_delete(lsm->run_hist);
	vy_lsm_stat_destroy(&lsm->stat);
	vy_cache_destroy(&lsm->cache);
	tuple_format_unref(lsm->mem_format);
	TRASH(lsm);
	free(lsm);
}

int
vy_lsm_create(struct vy_lsm *lsm)
{
	/*
	 * Allocate a unique id for the new LSM tree, but don't assign
	 * it until information about the new LSM tree is successfully
	 * written to vylog as vinyl_index_abort_create() uses id to
	 * decide whether it needs to clean up.
	 */
	int64_t id = vy_log_next_id();

	/* Create the initial range: the whole key space. */
	struct vy_range *range = vy_range_new(vy_log_next_id(),
					      lsm->env->key_inf,
					      lsm->env->key_sup,
					      lsm->cmp_def);
	if (range == NULL)
		return -1;
	assert(lsm->range_count == 0);
	vy_lsm_add_range(lsm, range);

	/* Write the new LSM tree record to vylog. */
	vy_log_tx_begin();
	vy_log_prepare_lsm(id, lsm->space_id, lsm->index_id,
			   lsm->group_id, lsm->key_def);
	vy_log_insert_range(id, range->id, range->begin.stmt,
			    range->end.stmt);
	vy_log_tx_try_commit();

	/* Assign the id. */
	assert(lsm->id < 0);
	lsm->id = id;
	return 0;
}

static struct vy_run *
vy_lsm_recover_run(struct vy_lsm *lsm, struct vy_run_recovery_info *run_info,
		   struct vy_run_env *run_env, bool force_recovery)
{
	assert(!run_info->is_dropped);
	assert(!run_info->is_incomplete);

	if (run_info->data != NULL) {
		/* Already recovered. */
		return run_info->data;
	}

	struct vy_dict *dict = NULL;
	if (run_info->dict_id > 0) {
		dict = vy_lsm_lookup_dict(lsm, run_info->dict_id);
		if (dict == NULL) {
			diag_set(ClientError, ER_INVALID_VYLOG_FILE,
				 tt_sprintf("dictionary %lld not found "
					    "for run %lld",
					    (long long)run_info->dict_id,
					    (long long)run_info->id));
			return NULL;
		}
	}
	struct vy_run *run = vy_run_new(run_env, run_info->id, dict);
	if (run == NULL)
		return NULL;

	run->dump_lsn = run_info->dump_lsn;
	run->dump_count = run_info->dump_count;
	if (vy_run_recover(run, lsm->env->path, lsm->space_id, lsm->index_id,
			   lsm->cmp_def) != 0 &&
	    (!force_recovery ||
	     vy_run_rebuild_index(run, lsm->env->path,
				  lsm->space_id, lsm->index_id,
				  lsm->cmp_def, lsm->key_def,
				  lsm->disk_format, &lsm->opts) != 0)) {
		vy_run_unref(run);
		return NULL;
	}
	vy_lsm_add_run(lsm, run);

	/*
	 * The same run can be referenced by more than one slice
	 * so we cache recovered runs in run_info to avoid loading
	 * the same run multiple times.
	 *
	 * Runs are stored with their reference counters elevated.
	 * We drop the extra references as soon as LSM tree recovery
	 * is complete (see vy_lsm_recover()).
	 */
	run_info->data = run;
	return run;
}

static struct vy_slice *
vy_lsm_recover_slice(struct vy_lsm *lsm, struct vy_range *range,
		     struct vy_slice_recovery_info *slice_info,
		     struct vy_run_env *run_env, bool force_recovery)
{
	struct vy_entry begin = vy_entry_none();
	struct vy_entry end = vy_entry_none();
	struct vy_slice *slice = NULL;
	struct vy_run *run;

	if (slice_info->begin != NULL) {
		begin = vy_entry_key_from_msgpack(lsm->env->key_format,
						  lsm->cmp_def,
						  slice_info->begin);
		if (begin.stmt == NULL)
			goto out;
	}
	if (slice_info->end != NULL) {
		end = vy_entry_key_from_msgpack(lsm->env->key_format,
						lsm->cmp_def,
						slice_info->end);
		if (end.stmt == NULL)
			goto out;
	}
	if (begin.stmt != NULL && end.stmt != NULL &&
	    vy_entry_compare(begin, end, lsm->cmp_def) >= 0) {
		diag_set(ClientError, ER_INVALID_VYLOG_FILE,
			 tt_sprintf("begin >= end for slice %lld",
				    (long long)slice_info->id));
		goto out;
	}

	run = vy_lsm_recover_run(lsm, slice_info->run,
				 run_env, force_recovery);
	if (run == NULL)
		goto out;

	slice = vy_slice_new(slice_info->id, run);
	if (slice == NULL)
		goto out;

	vy_range_add_slice(range, slice);
out:
	if (begin.stmt != NULL)
		tuple_unref(begin.stmt);
	if (end.stmt != NULL)
		tuple_unref(end.stmt);
	return slice;
}

static struct vy_range *
vy_lsm_recover_range(struct vy_lsm *lsm,
		     struct vy_range_recovery_info *range_info,
		     struct vy_run_env *run_env, bool force_recovery)
{
	struct vy_range *range = NULL;
	/*
	 * An absent key in the log is the key space bound; an
	 * interior key becomes the infimum bound it has always
	 * meant: an inclusive begin, an exclusive end.
	 */
	struct vy_entry begin = lsm->env->key_inf;
	struct vy_entry end = lsm->env->key_sup;
	tuple_ref(begin.stmt);
	tuple_ref(end.stmt);

	if (range_info->begin != NULL) {
		tuple_unref(begin.stmt);
		begin = vy_entry_key_from_msgpack(lsm->env->key_format,
						  lsm->cmp_def,
						  range_info->begin);
		if (begin.stmt == NULL) {
			begin = vy_entry_none();
			goto out;
		}
		vy_stmt_add_flag(begin.stmt, VY_STMT_INFIMUM);
	}
	if (range_info->end != NULL) {
		tuple_unref(end.stmt);
		end = vy_entry_key_from_msgpack(lsm->env->key_format,
						lsm->cmp_def,
						range_info->end);
		if (end.stmt == NULL) {
			end = vy_entry_none();
			goto out;
		}
		vy_stmt_add_flag(end.stmt, VY_STMT_INFIMUM);
	}
	if (vy_bound_cmp(begin, end, lsm->cmp_def) >= 0) {
		diag_set(ClientError, ER_INVALID_VYLOG_FILE,
			 tt_sprintf("begin >= end for range %lld",
				    (long long)range_info->id));
		goto out;
	}

	range = vy_range_new(range_info->id, begin, end, lsm->cmp_def);
	if (range == NULL)
		goto out;

	/*
	 * Newer slices are stored closer to the head of the list,
	 * while we are supposed to add slices in chronological
	 * order, so use reverse iterator.
	 */
	struct vy_slice_recovery_info *slice_info;
	rlist_foreach_entry_reverse(slice_info, &range_info->slices, in_range) {
		if (vy_lsm_recover_slice(lsm, range, slice_info,
					 run_env, force_recovery) == NULL) {
			vy_range_delete(range);
			range = NULL;
			goto out;
		}
	}
	vy_lsm_add_range(lsm, range);
out:
	if (begin.stmt != NULL)
		tuple_unref(begin.stmt);
	if (end.stmt != NULL)
		tuple_unref(end.stmt);
	return range;
}

int
vy_lsm_recover(struct vy_lsm *lsm, struct vy_recovery *recovery,
		 struct vy_run_env *run_env, int64_t lsn,
		 bool is_checkpoint_recovery, bool force_recovery)
{
	assert(lsm->id < 0);
	assert(lsm->commit_lsn < 0);
	assert(lsm->range_count == 0);

	/*
	 * Backward compatibility fixup: historically, we used
	 * box.info.signature for LSN of index creation, which
	 * lags behind the LSN of the record that created the
	 * index by 1. So for legacy indexes use the LSN from
	 * index options.
	 */
	if (lsm->opts.lsn != 0)
		lsn = lsm->opts.lsn;

	/*
	 * Look up the last incarnation of the LSM tree in vylog.
	 */
	struct vy_lsm_recovery_info *lsm_info;
	lsm_info = vy_recovery_lsm_by_index_id(recovery,
			lsm->space_id, lsm->index_id);
	if (is_checkpoint_recovery) {
		if (lsm_info == NULL || lsm_info->create_lsn < 0) {
			/*
			 * All LSM trees created from snapshot rows must
			 * be present in vylog, because snapshot can
			 * only succeed if vylog has been successfully
			 * flushed.
			 */
			diag_set(ClientError, ER_INVALID_VYLOG_FILE,
				 tt_sprintf("LSM tree %u/%u not found",
					    (unsigned)lsm->space_id,
					    (unsigned)lsm->index_id));
			return -1;
		}
		if (lsn > lsm_info->create_lsn) {
			/*
			 * The last incarnation of the LSM tree was
			 * created before the last checkpoint, load
			 * it now.
			 */
			lsn = lsm_info->create_lsn;
		}
	}

	if (lsm_info == NULL || (lsm_info->prepared == NULL &&
				 lsm_info->create_lsn >= 0 &&
				 lsn > lsm_info->create_lsn)) {
		/*
		 * If we failed to log LSM tree creation before restart,
		 * we won't find it in the log on recovery. This is OK as
		 * the LSM tree doesn't have any runs in this case. We will
		 * retry to log LSM tree in vinyl_index_commit_create().
		 * For now, just create the initial range and assign id.
		 *
		 * Note, this is needed only for backward compatibility
		 * since now we write VY_LOG_PREPARE_LSM before WAL write
		 * and hence if the index was committed to WAL, it must be
		 * present in vylog as well.
		 */
		return vy_lsm_create(lsm);
	}

	if (lsm_info->create_lsn >= 0 && lsn > lsm_info->create_lsn) {
		/*
		 * The index we are recovering was prepared, successfully
		 * built, and committed to WAL, but it was not marked as
		 * created in vylog. Recover the prepared LSM tree. We will
		 * retry vylog write in vinyl_index_commit_create().
		 */
		lsm_info = lsm_info->prepared;
		assert(lsm_info != NULL);
	}

	lsm->id = lsm_info->id;
	lsm->commit_lsn = lsm_info->modify_lsn;

	if (lsn < lsm_info->create_lsn || lsm_info->drop_lsn >= 0) {
		/*
		 * Loading a past incarnation of the LSM tree, i.e.
		 * the LSM tree is going to dropped during final
		 * recovery. Mark it as such.
		 */
		lsm->is_dropped = true;
		/*
		 * We need range tree initialized for all LSM trees,
		 * even for dropped ones.
		 */
		struct vy_range *range;
		range = vy_range_new(vy_log_next_id(), lsm->env->key_inf,
				     lsm->env->key_sup, lsm->cmp_def);
		if (range == NULL)
			return -1;
		vy_lsm_add_range(lsm, range);
		return 0;
	}

	/*
	 * Loading the last incarnation of the LSM tree from vylog.
	 */
	lsm->dump_lsn = lsm_info->dump_lsn;

	/*
	 * Recover dictionaries from the LSM tree's dict list.
	 * Only dicts with dict_refs > 0 are present (zero-ref
	 * dicts are freed in vy_recovery_forget_run).
	 */
	struct vy_dict_recovery_info *dict_info;
	for (dict_info = vy_recovery_dict_tree_first(&lsm_info->dicts);
	     dict_info != NULL;
	     dict_info = vy_recovery_dict_tree_next(&lsm_info->dicts,
						    dict_info)) {
		assert(dict_info->dict_refs > 0);
		struct vy_dict *dict;
		dict = vy_dict_new(dict_info->id, dict_info->dict_data,
				   dict_info->dict_size,
				   lsm->opts.compression_level);
		if (dict == NULL)
			return -1;
		vy_lsm_add_dict(lsm, dict);
	}

	int rc = 0;
	struct vy_range_recovery_info *range_info;
	rlist_foreach_entry(range_info, &lsm_info->ranges, in_lsm) {
		if (vy_lsm_recover_range(lsm, range_info, run_env,
					 force_recovery) == NULL) {
			rc = -1;
			break;
		}
	}

	/*
	 * vy_lsm_recover_run() elevates reference counter
	 * of each recovered run. We need to drop the extra
	 * references once we are done.
	 */
	struct vy_run *run, *next_run;
	rlist_foreach_entry_safe(run, &lsm->runs, in_lsm, next_run) {
		/*
		 * In case vy_lsm_recover_range() failed, slices
		 * are already deleted and runs are unrefed. So
		 * we have nothing to do but finish run clean-up.
		 */
		if (run->refs == 1) {
			assert(rc != 0);
			assert(run->slice_count == 0);
			vy_lsm_remove_run(lsm, run);
		}
		vy_run_unref(run);
	}

	if (rc != 0)
		return -1;

	/*
	 * Set the active dictionary from the most recent run so
	 * that new runs reuse it when no freshly trained dictionary
	 * is available. If the newest run has no dict, we do not
	 * fall back to an older run's dict: the data distribution
	 * may have shifted enough that the old dict would compress
	 * poorly, and the next dump will train a fresh one anyway.
	 *
	 * We cannot simply take the first entry of lsm->runs
	 * because recovery order depends on the range/slice
	 * iteration order, not on run ids. Find the run with the
	 * greatest id explicitly.
	 */
	struct vy_run *newest_run = NULL;
	rlist_foreach_entry(run, &lsm->runs, in_lsm) {
		if (newest_run == NULL || run->id > newest_run->id)
			newest_run = run;
	}
	if (newest_run != NULL && newest_run->dict != NULL) {
		assert(lsm->dict_last.dict == NULL);
		lsm->dict_last.dict = newest_run->dict;
		vy_dict_ref(newest_run->dict);
	}

	/* Check that the range tree does not have holes or overlaps. */
	struct vy_range *range, *prev = NULL;
	for (range = vy_range_tree_first(&lsm->range_tree); range != NULL;
	     prev = range, range = vy_range_tree_next(&lsm->range_tree, range)) {
		if (prev == NULL &&
		    !vy_stmt_is_empty_key(range->begin.stmt)) {
			diag_set(ClientError, ER_INVALID_VYLOG_FILE,
				 tt_sprintf("Range %lld is leftmost but "
					    "starts with a finite key",
					    (long long)range->id));
			return -1;
		}
		int cmp = 0;
		if (prev != NULL &&
		    (cmp = vy_bound_cmp(prev->end, range->begin,
					lsm->cmp_def)) != 0) {
			const char *errmsg = cmp > 0 ?
				"Nearby ranges %lld and %lld overlap" :
				"Keys between ranges %lld and %lld not spanned";
			diag_set(ClientError, ER_INVALID_VYLOG_FILE,
				 tt_sprintf(errmsg,
					    (long long)prev->id,
					    (long long)range->id));
			return -1;
		}
	}
	if (prev == NULL) {
		diag_set(ClientError, ER_INVALID_VYLOG_FILE,
			 tt_sprintf("LSM tree %lld has empty range tree",
				    (long long)lsm->id));
		return -1;
	}
	if (!vy_stmt_is_empty_key(prev->end.stmt)) {
		diag_set(ClientError, ER_INVALID_VYLOG_FILE,
			 tt_sprintf("Range %lld is rightmost but "
				    "ends with a finite key",
				    (long long)prev->id));
		return -1;
	}
	return 0;
}

int64_t
vy_lsm_generation(struct vy_lsm *lsm)
{
	struct vy_mem *oldest = rlist_empty(&lsm->sealed) ? lsm->mem :
		rlist_last_entry(&lsm->sealed, struct vy_mem, in_mems);
	return oldest->generation;
}

int
vy_lsm_compaction_priority(struct vy_lsm *lsm)
{
	struct vy_range *range = vy_range_heap_top(&lsm->range_heap);
	if (range == NULL)
		return 0;
	/*
	 * There's no point in compacting dropped LSM trees. Moreover, since we
	 * don't commit a new run for a dropped LSM tree so as not to mess with
	 * garbage collection (see vy_task_compaction_complete()), enabling
	 * compaction in this case would result in rescheduling it over and
	 * over again, which is no good.
	 */
	if (lsm->is_dropped)
		return 0;
	return range->compaction_plan.priority;
}

int64_t
vy_lsm_range_size(struct vy_lsm *lsm)
{
	/*
	 * Use the configured range size, or the compile-time
	 * minimum if not set.
	 *
	 * The old heuristic that derived the range size from
	 * dumps_per_compaction and last-level bytes is removed:
	 * a fixed default is simpler and avoids the feedback
	 * loops that caused the heuristic to over-shrink ranges
	 * in skewed workloads, leading to excessive compaction.
	 *
	 * Cap at VY_MAX_RANGE_SIZE so that a single compaction
	 * job finishes in reasonable time.
	 */
	if (lsm->opts.range_size > 0)
		return MIN(lsm->opts.range_size, VY_MAX_RANGE_SIZE);
	return VY_MIN_RANGE_SIZE;
}

void
vy_lsm_add_run(struct vy_lsm *lsm, struct vy_run *run)
{
	struct vy_lsm_env *env = lsm->env;
	size_t bloom_size = vy_run_bloom_size(run);
	size_t page_index_size = run->page_index_size;

	assert(rlist_empty(&run->in_lsm));
	rlist_add_entry(&lsm->runs, run, in_lsm);
	lsm->run_count++;
	vy_disk_stmt_counter_add(&lsm->stat.disk.count, &run->count);
	vy_stmt_stat_add(&lsm->stat.disk.stmt, &run->info.stmt_stat);

	lsm->bloom_size += bloom_size;
	lsm->page_index_size += page_index_size;
	lsm->lcp_total_prefix += run->lcp_index.total_prefix_len;
	lsm->lcp_group_count += run->lcp_index.count;

	env->bloom_size += bloom_size;
	env->page_index_size += page_index_size;

	/* Data size is consistent with space.bsize. */
	if (lsm->index_id == 0)
		env->disk_data_size += run->count.bytes;
	/* Index size is consistent with index.bsize. */
	env->disk_index_size += bloom_size + page_index_size;
	if (lsm->index_id > 0)
		env->disk_index_size += run->count.bytes;
}

void
vy_lsm_remove_run(struct vy_lsm *lsm, struct vy_run *run)
{
	struct vy_lsm_env *env = lsm->env;
	size_t bloom_size = vy_run_bloom_size(run);
	size_t page_index_size = run->page_index_size;

	assert(lsm->run_count > 0);
	assert(!rlist_empty(&run->in_lsm));
	rlist_del_entry(run, in_lsm);
	lsm->run_count--;
	vy_disk_stmt_counter_sub(&lsm->stat.disk.count, &run->count);
	vy_stmt_stat_sub(&lsm->stat.disk.stmt, &run->info.stmt_stat);

	lsm->bloom_size -= bloom_size;
	lsm->page_index_size -= page_index_size;
	lsm->lcp_total_prefix -= run->lcp_index.total_prefix_len;
	lsm->lcp_group_count -= run->lcp_index.count;

	env->bloom_size -= bloom_size;
	env->page_index_size -= page_index_size;

	/* Data size is consistent with space.bsize. */
	if (lsm->index_id == 0)
		env->disk_data_size -= run->count.bytes;
	/* Index size is consistent with index.bsize. */
	env->disk_index_size -= bloom_size + page_index_size;
	if (lsm->index_id > 0)
		env->disk_index_size -= run->count.bytes;
}

void
vy_lsm_add_range(struct vy_lsm *lsm, struct vy_range *range)
{
	assert(heap_node_is_stray(&range->heap_node));
	vy_range_heap_insert(&lsm->range_heap, range);
	vy_range_tree_insert(&lsm->range_tree, range);
	lsm->range_count++;
	lsm->range_tree_version++;
	vy_range_update_compaction_priority(range, &lsm->opts,
					    vy_lsm_range_size(lsm));
	vy_lsm_acct_range(lsm, range);
	vy_range_heap_update(&lsm->range_heap, range);
}

void
vy_lsm_remove_range(struct vy_lsm *lsm, struct vy_range *range)
{
	assert(! heap_node_is_stray(&range->heap_node));
	vy_range_heap_delete(&lsm->range_heap, range);
	vy_range_tree_remove(&lsm->range_tree, range);
	if (lsm->last_range == range)
		lsm->last_range = NULL;
	lsm->range_count--;
}

void
vy_lsm_acct_range(struct vy_lsm *lsm, struct vy_range *range)
{
	histogram_collect(lsm->run_hist, range->slice_count);
	lsm->sum_dumps_per_compaction += range->dumps_per_compaction;
	vy_disk_stmt_counter_add(&lsm->stat.disk.compaction.queue,
				 &range->compaction_queue);
	lsm->env->compaction_queue_size += range->compaction_queue.bytes;
	if (!rlist_empty(&range->slices)) {
		struct vy_slice *slice = rlist_last_entry(&range->slices,
						struct vy_slice, in_range);
		vy_disk_stmt_counter_add(&lsm->stat.disk.last_level_count,
					 &slice->count);
		if (lsm->index_id == 0)
			lsm->env->compacted_data_size += slice->count.bytes;
	}
}

void
vy_lsm_unacct_range(struct vy_lsm *lsm, struct vy_range *range)
{
	histogram_discard(lsm->run_hist, range->slice_count);
	lsm->sum_dumps_per_compaction -= range->dumps_per_compaction;
	vy_disk_stmt_counter_sub(&lsm->stat.disk.compaction.queue,
				 &range->compaction_queue);
	lsm->env->compaction_queue_size -= range->compaction_queue.bytes;
	if (!rlist_empty(&range->slices)) {
		struct vy_slice *slice = rlist_last_entry(&range->slices,
						struct vy_slice, in_range);
		vy_disk_stmt_counter_sub(&lsm->stat.disk.last_level_count,
					 &slice->count);
		if (lsm->index_id == 0)
			lsm->env->compacted_data_size -= slice->count.bytes;
	}
}

void
vy_lsm_acct_read_amp(struct vy_lsm *lsm, struct vy_range *range,
		     int64_t disk_bytes, struct tuple *result)
{
	if (disk_bytes == 0 || range == NULL)
		return;
	/*
	 * Skip both accumulation and the threshold check if we
	 * already triggered compaction for this range version.
	 * Re-compacting unchanged slices cannot reduce the waste,
	 * and continuing to accumulate stale bytes_read would
	 * inflate the waste counter, causing a spurious re-trigger
	 * after the next version bump (e.g. a dump).
	 */
	if (range->read_amp.threshold_version == range->version)
		return;
	vy_range_acct_read_amp(range, disk_bytes, result);
	int64_t waste = range->read_amp.bytes_read -
			range->read_amp.bytes_useful;
	if (waste <= range->count.bytes * VY_READ_AMP_THRESHOLD)
		return;
	/*
	 * The waste has crossed the threshold.  Invoke the
	 * callback to recompute the compaction priority and
	 * reschedule the LSM tree.
	 *
	 * The callback calls vy_lsm_update_range ->
	 * vy_compaction_plan_check_read_amp, which resets
	 * the stats and sets threshold_version to suppress
	 * further triggers until the slice set changes.
	 *
	 * Zero the counters again after the callback as a
	 * safety net: if compaction completes during a scan
	 * yield and reduces the range to <= 1 slice,
	 * check_read_amp returns early without resetting.
	 * The redundant reset for the normal path is harmless.
	 */
	if (lsm->env->compaction_trigger_cb != NULL)
		lsm->env->compaction_trigger_cb(lsm, range,
				lsm->env->compaction_trigger_arg);
	vy_read_amp_stat_reset(&range->read_amp);
}

void
vy_lsm_update_range(struct vy_lsm *lsm, struct vy_range *range,
		    struct vy_slice *add_slice, struct vy_slice **del_slices)
{
	vy_lsm_unacct_range(lsm, range);
	if (add_slice != NULL) {
		if (del_slices) {
			vy_range_add_slice_before(range, add_slice,
						  *del_slices);
		} else {
			vy_range_add_slice(range, add_slice);
		}
	}
	if (del_slices && *del_slices) {
		range->n_compactions++;
		while (*del_slices) {
			vy_range_remove_slice(range, *del_slices++);
		}
	}
	vy_range_update_compaction_priority(range, &lsm->opts,
					    vy_lsm_range_size(lsm));
	vy_lsm_acct_range(lsm, range);
	/*
	 * The range is removed from the LSM heap during compaction.
	 * A dump or forced compaction may happen concurrently.
	 */
	if (!heap_node_is_stray(&range->heap_node)) {
		vy_range_heap_update(&lsm->range_heap, range);
	}
	/*
	 * If the new slice is last, reset the blind write probe
	 * stats built against the old bloom filter.
	 */
	if (add_slice != NULL &&
	    rlist_last_entry(&range->slices, struct vy_slice,
			     in_range) == add_slice)
		vy_blind_write_stat_reset(&lsm->stat.blind);
}

void
vy_lsm_debloat(struct vy_lsm *lsm, struct vy_range *start,
	       struct vy_run *run, bool forward)
{
	typedef struct vy_range *(*vy_range_step_f)(
		vy_range_tree_t *, struct vy_range *);
	vy_range_step_f step = forward
		? vy_range_tree_next : vy_range_tree_prev;

	for (struct vy_range *r = step(&lsm->range_tree, start); r != NULL;
	     r = step(&lsm->range_tree, r)) {
		/*
		 * Boundary check: range->begin is inclusive,
		 * range->end is exclusive.
		 */
		if (forward) {
			if (!vy_stmt_is_empty_key(r->begin.stmt) &&
			    vy_entry_compare_with_raw_key(
					r->begin, run->info.max_key,
					HINT_NONE, lsm->cmp_def) > 0)
				return;
		} else {
			if (!vy_stmt_is_empty_key(r->end.stmt) &&
			    vy_entry_compare_with_raw_key(
					r->end, run->info.min_key,
					HINT_NONE, lsm->cmp_def) <= 0)
				return;
		}
		if (vy_range_has_run(r, run)) {
			if (!heap_node_is_stray(&r->heap_node) &&
			    r->compaction_plan.priority == 0)
				vy_lsm_update_range(lsm, r, NULL, NULL);
			return;
		}
	}
}

void
vy_lsm_acct_dump(struct vy_lsm *lsm, double time,
		 const struct vy_stmt_counter *input,
		 const struct vy_disk_stmt_counter *output)
{
	lsm->stat.disk.dump.count++;
	lsm->stat.disk.dump.time += time;
	vy_stmt_counter_add(&lsm->stat.disk.dump.input, input);
	vy_disk_stmt_counter_add(&lsm->stat.disk.dump.output, output);
}

void
vy_lsm_acct_compaction(struct vy_lsm *lsm, double time,
		       const struct vy_disk_stmt_counter *input,
		       const struct vy_disk_stmt_counter *output)
{
	lsm->stat.disk.compaction.count++;
	lsm->stat.disk.compaction.time += time;
	vy_disk_stmt_counter_add(&lsm->stat.disk.compaction.input, input);
	vy_disk_stmt_counter_add(&lsm->stat.disk.compaction.output, output);
}

int
vy_lsm_rotate_mem(struct vy_lsm *lsm)
{
	struct vy_mem *mem;

	assert(lsm->mem != NULL);
	mem = vy_mem_new(lsm->mem->env, &lsm->stat.memory, lsm->cmp_def,
			 lsm->mem_format, *lsm->env->p_generation,
			 space_cache_version, lsm->index_id);
	if (mem == NULL)
		return -1;

	assert(lsm->mem->state == VY_MEM_ACTIVE);
	lsm->mem->state = VY_MEM_SEALED;
	rlist_add_entry(&lsm->sealed, lsm->mem, in_mems);
	lsm->mem = mem;
	lsm->mem_list_version++;
	return 0;
}

int
vy_lsm_rotate_mem_if_required(struct vy_lsm *lsm)
{
	if (lsm->mem->space_cache_version != space_cache_version ||
	    lsm->mem->generation != *lsm->env->p_generation) {
		if (vy_lsm_rotate_mem(lsm) != 0)
			return -1;
	}
	return 0;
}

void
vy_lsm_delete_mem(struct vy_lsm *lsm, struct vy_mem *mem)
{
	assert(mem->state == VY_MEM_SEALED);
	vy_mem_delete(mem);
	lsm->mem_list_version++;
}

/**
 * Sample the last-level bloom filter for this key. A hit means
 * the key is already on disk: a concurrent blind write is an
 * overwrite (no new key added) and a concurrent blind delete
 * removes a live tuple. A miss means the opposite -- the write
 * introduces a new key, the delete is a phantom. space:len()
 * uses the accumulated hit/miss ratio to estimate the net
 * effect of blind operations.
 */
void
vy_lsm_probe_blind_write(struct vy_lsm *lsm, struct tuple *stmt)
{
	assert(lsm->index_id == 0);
	int64_t n = ++lsm->stat.blind.attempts;
	if (n > VY_BLIND_WRITE_SAMPLE_RATE && n % VY_BLIND_WRITE_SAMPLE_RATE != 0)
		return;
	struct vy_entry entry = {
		.stmt = stmt,
		.hint = vy_stmt_hint(stmt, lsm->key_def),
	};
	struct vy_range *range = vy_range_tree_find_by_key(
		&lsm->range_tree, entry);
	if (rlist_empty(&range->slices))
		return;
	struct vy_slice *last = rlist_last_entry(&range->slices,
						 struct vy_slice, in_range);
	if (last->run->info.bloom == NULL)
		return;
	if (vy_bloom_maybe_has(last->run->info.bloom, entry, lsm->key_def))
		lsm->stat.blind.write_hit++;
	else
		lsm->stat.blind.write_miss++;
}

int
vy_lsm_set(struct vy_lsm *lsm, struct vy_mem *mem,
	   struct vy_entry entry, struct vy_entry *mem_stmt,
	   enum vy_quota_consumer_type consumer, struct vy_entry *prev)
{
	uint32_t format_id = entry.stmt->format_id;

	/* Abort transaction if format was changed by DDL */
	if (!vy_stmt_is_key(entry.stmt) &&
	    format_id != tuple_format_id(mem->format)) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}

	/* Invalidate cache element. */
	struct vy_entry deleted = vy_entry_none();
	vy_cache_on_write(&lsm->cache, entry, &deleted);

	/*
	 * The UPSERT statement can be applied to the cached statement,
	 * because the cache always contains newest REPLACE statements.
	 * In such a case the UPSERT, applied to the cached statement,
	 * can be inserted instead of the original UPSERT.
	 */
	struct vy_entry insert = entry;
	if (deleted.stmt != NULL &&
	    vy_stmt_type(entry.stmt) == IPROTO_UPSERT) {
		struct vy_entry applied = vy_entry_apply_upsert(
				entry, deleted, mem->cmp_def, false);
		/*
		 * Ignore a memory error, because it is not critical
		 * to apply the optimization.
		 */
		if (applied.stmt != NULL)
			insert = applied;
	}
	if (deleted.stmt != NULL)
		vy_stmt_unref(deleted.stmt);

	int rc;
	if (vy_stmt_type(insert.stmt) != IPROTO_UPSERT)
		rc = vy_mem_insert(mem, insert, consumer, prev);
	else
		rc = vy_mem_insert_upsert(mem, insert, consumer, prev);
	/*
	 * If we folded the UPSERT, the merged REPLACE carries our
	 * creation ref and the mem took its own -- drop ours. The
	 * original entry's ref belongs to the caller.
	 */
	if (insert.stmt != entry.stmt)
		vy_stmt_unref(insert.stmt);
	if (rc == 0)
		*mem_stmt = insert;
	return rc;
}

int64_t
vy_lsm_complete_dump(struct vy_lsm *lsm, int64_t dump_lsn,
		     int64_t dump_generation, double dump_time,
		     const struct vy_disk_stmt_counter *dump_output)
{
	/*
	 * Update dump_lsn before freeing the sealed mems so
	 * that vy_lsm_check_concurrent_write can proceed
	 * to the disk scan if a sealed mem is freed but its
	 * entries are now on disk.
	 */
	lsm->dump_lsn = MAX(lsm->dump_lsn, dump_lsn);
	vy_tx_manager_check_concurrent_write(lsm->mem->env->xm, lsm,
					     dump_lsn, dump_generation);
	struct vy_stmt_counter dump_input;
	vy_stmt_counter_reset(&dump_input);
	struct vy_mem *mem, *next_mem;
	rlist_foreach_entry_safe(mem, &lsm->sealed, in_mems, next_mem) {
		if (mem->generation > dump_generation)
			continue;
		vy_stmt_counter_add(&dump_input, &mem->stat.count);
		vy_lsm_delete_mem(lsm, mem);
	}
	vy_lsm_acct_dump(lsm, dump_time, &dump_input, dump_output);
	lsm->is_dumping = false;
	/*
	 * Indexes of a space share a memory level, so only the primary
	 * index's input is accounted; return 0 for secondary indexes.
	 */
	return lsm->index_id == 0 ? dump_input.bytes : 0;
}

/**
 * Calculate and record the number of sequential upserts, squash
 * immediately or schedule upsert process if needed.
 * Additional handler used in vy_lsm_commit_stmt() for UPSERT
 * statements.
 *
 * @param lsm   LSM tree the statement was committed to.
 * @param mem   In-memory tree where the statement was saved.
 * @param entry UPSERT statement to squash.
 */
static void
vy_lsm_commit_upsert(struct vy_lsm *lsm, struct vy_mem *mem,
		     struct vy_entry entry)
{
	assert(vy_stmt_type(entry.stmt) == IPROTO_UPSERT);
	assert(!vy_stmt_is_prepared(entry.stmt));
	/*
	 * UPSERT is enabled only for the spaces with the single
	 * index.
	 */
	assert(lsm->index_id == 0);

	uint8_t n_upserts = vy_stmt_n_upserts(entry.stmt);
	/*
	 * If there are a lot of successive upserts for the same key,
	 * select might take too long to squash them all. So once the
	 * number of upserts exceeds a certain threshold, we schedule
	 * a fiber to merge them and insert the resulting statement
	 * after the latest upsert.
	 */
	if (n_upserts == VY_UPSERT_INF) {
		/*
		 * If UPSERT has n_upserts > VY_UPSERT_THRESHOLD,
		 * it means the mem has older UPSERTs for the same
		 * key which already are beeing processed in the
		 * squashing task. At the end, the squashing task
		 * will merge its result with this UPSERT
		 * automatically.
		 */
		return;
	}
	if (n_upserts == VY_UPSERT_THRESHOLD) {
		/*
		 * Start single squashing task per one-mem and
		 * one-key continous UPSERTs sequence.
		 */
#ifndef NDEBUG
		struct vy_entry older = vy_mem_older_lsn(mem, entry);
		assert(older.stmt != NULL &&
		       vy_stmt_type(older.stmt) == IPROTO_UPSERT &&
		       vy_stmt_n_upserts(older.stmt) == VY_UPSERT_THRESHOLD - 1);
#else
		(void)mem;
#endif
		if (lsm->env->upsert_thresh_cb == NULL) {
			/* Squash callback is not installed. */
			return;
		}

		struct vy_entry dup;
		dup.hint = entry.hint;
		dup.stmt = vy_stmt_dup(entry.stmt);
		if (dup.stmt != NULL) {
			lsm->env->upsert_thresh_cb(lsm, dup,
					lsm->env->upsert_thresh_arg);
			tuple_unref(dup.stmt);
		}
		/*
		 * Ignore dup == NULL, because the optimization is
		 * good, but is not necessary.
		 */
		return;
	}
}

void
vy_lsm_commit_stmt(struct vy_lsm *lsm, struct vy_mem *mem,
		   struct vy_entry entry)
{
	vy_mem_commit_stmt(mem, entry);

	if (vy_stmt_type(entry.stmt) == IPROTO_UPSERT)
		vy_lsm_commit_upsert(lsm, mem, entry);

	vy_stmt_counter_acct_tuple(&lsm->stat.put, entry.stmt);

	/*
	 * No cache invalidation here: the cache was invalidated when
	 * the statement was prepared, and unconfirmed data never
	 * enters the cache -- not as an entry and not as anything a
	 * link attaches to (see vy_cache_insert()). Confirmation
	 * therefore cannot falsify any cache content.
	 */
}

void
vy_lsm_rollback_stmt(struct vy_lsm *lsm, struct vy_mem *mem,
		     struct vy_entry entry)
{
	vy_mem_rollback_stmt(mem, entry);
	vy_cache_on_rollback(&lsm->cache, entry);
}

int
vy_lsm_find_range_intersection(struct vy_lsm *lsm,
		const char *min_key, const char *max_key,
		struct vy_range **begin, struct vy_range **end)
{
	struct tuple_format *key_format = lsm->env->key_format;
	struct vy_entry entry;

	entry = vy_entry_key_from_msgpack(key_format, lsm->cmp_def, min_key);
	if (entry.stmt == NULL)
		return -1;
	*begin = vy_range_tree_psearch(&lsm->range_tree, entry);
	tuple_unref(entry.stmt);

	entry = vy_entry_key_from_msgpack(key_format, lsm->cmp_def, max_key);
	if (entry.stmt == NULL)
		return -1;
	*end = vy_range_tree_psearch(&lsm->range_tree, entry);
	*end = vy_range_tree_next(&lsm->range_tree, *end);
	tuple_unref(entry.stmt);

	return 0;
}

bool
vy_lsm_split_range(struct vy_lsm *lsm, struct vy_range *range,
		   const char *split_key_raw)
{
	struct tuple_format *key_format = lsm->env->key_format;

	/* Split a range in two parts. */
	const int n_parts = 2;
	struct vy_range *parts[2] = { NULL, NULL };
	/*
	 * Determine new ranges' boundaries.
	 */
	struct vy_entry split_key;
	split_key = vy_entry_key_from_msgpack(key_format, lsm->cmp_def,
					      split_key_raw);
	if (split_key.stmt == NULL)
		goto fail;
	/*
	 * The split key is the left part's exclusive end and the
	 * right part's inclusive begin: one infimum bound serves
	 * both, and the slices inherit it verbatim.
	 */
	vy_stmt_add_flag(split_key.stmt, VY_STMT_INFIMUM);

	struct vy_entry keys[3];
	keys[0] = range->begin;
	keys[1] = split_key;
	keys[2] = range->end;

	/*
	 * Allocate new ranges and create slices of
	 * the old range's runs for them.
	 */
	struct vy_slice *slice, *new_slice;
	struct vy_range *part = NULL;
	for (int i = 0; i < n_parts; i++) {
		part = vy_range_new(vy_log_next_id(), keys[i], keys[i + 1],
				    lsm->cmp_def);
		if (part == NULL)
			goto fail;
		parts[i] = part;
		/*
		 * vy_range_add_slice() adds a slice to the list head,
		 * so to preserve the order of the slices list, we have
		 * to iterate backward.
		 */
		rlist_foreach_entry_reverse(slice, &range->slices, in_range) {
			if (vy_slice_cut(slice, vy_log_next_id(), part->begin,
					 part->end, lsm->cmp_def,
					 &new_slice) != 0)
				goto fail;
			if (new_slice != NULL)
				vy_range_add_slice(part, new_slice);
		}
	}

	/*
	 * Log change in metadata.
	 */
	vy_log_tx_begin();
	rlist_foreach_entry(slice, &range->slices, in_range)
		vy_log_delete_slice(slice->id);
	vy_log_delete_range(range->id);
	for (int i = 0; i < n_parts; i++) {
		part = parts[i];
		vy_log_insert_range(lsm->id, part->id,
				    part->begin.stmt, part->end.stmt);
		rlist_foreach_entry(slice, &part->slices, in_range)
			vy_log_insert_slice(part->id, slice->run->id,
					    slice->id, slice->begin.stmt,
					    part->end.stmt);
	}
	if (vy_log_tx_commit() < 0)
		goto fail;

	/*
	 * Replace the old range in the LSM tree.
	 */
	vy_lsm_unacct_range(lsm, range);
	vy_lsm_remove_range(lsm, range);

	for (int i = 0; i < n_parts; i++) {
		part = parts[i];
		part->needs_compaction = range->needs_compaction;
		vy_lsm_add_range(lsm, part);
	}

	say_verbose("%s: split range %s by key %s", vy_lsm_name(lsm),
		    vy_range_str(range), tuple_str(split_key.stmt));

	rlist_foreach_entry(slice, &range->slices, in_range)
		vy_slice_wait_pinned(slice);
	vy_range_delete(range);
	tuple_unref(split_key.stmt);
	return true;
fail:
	for (int i = 0; i < n_parts; i++) {
		if (parts[i] != NULL)
			vy_range_delete(parts[i]);
	}
	if (split_key.stmt != NULL)
		tuple_unref(split_key.stmt);

	diag_log();
	say_error("%s: failed to split range %s",
		  vy_lsm_name(lsm), vy_range_str(range));
	return false;
}

bool
vy_lsm_coalesce_range(struct vy_lsm *lsm, struct vy_range *range)
{
	struct vy_range *first, *last;
	if (!vy_range_needs_coalesce(range, &lsm->range_tree,
				     vy_lsm_range_size(lsm), &first, &last))
		return false;

	struct vy_range *result = vy_range_new(vy_log_next_id(),
			first->begin, last->end, lsm->cmp_def);
	if (result == NULL)
		goto fail_range;

	struct vy_range *it;
	struct vy_range *end = vy_range_tree_next(&lsm->range_tree, last);

	/*
	 * Log change in metadata.
	 */
	vy_log_tx_begin();
	vy_log_insert_range(lsm->id, result->id,
			    result->begin.stmt, result->end.stmt);
	for (it = first; it != end;
	     it = vy_range_tree_next(&lsm->range_tree, it)) {
		struct vy_slice *slice;
		rlist_foreach_entry(slice, &it->slices, in_range)
			vy_log_delete_slice(slice->id);
		vy_log_delete_range(it->id);
		rlist_foreach_entry(slice, &it->slices, in_range) {
			vy_log_insert_slice(result->id,
					    slice->run->id, slice->id,
					    slice->begin.stmt,
					    result->end.stmt);
		}
	}
	if (vy_log_tx_commit() < 0)
		goto fail_commit;

	/*
	 * Move run slices of the coalesced ranges to the
	 * resulting range and delete the former.
	 */
	it = first;
	while (it != end) {
		struct vy_range *next = vy_range_tree_next(&lsm->range_tree, it);
		vy_lsm_unacct_range(lsm, it);
		vy_lsm_remove_range(lsm, it);
		rlist_splice(&result->slices, &it->slices);
		result->slice_count += it->slice_count;
		vy_disk_stmt_counter_add(&result->count, &it->count);
		if (it->needs_compaction)
			result->needs_compaction = true;
		vy_range_delete(it);
		it = next;
	}
	/*
	 * Even though coalescing increases read amplification,
	 * we don't need to compact the resulting range as long
	 * as it fits the configured LSM tree shape.
	 */
	vy_lsm_add_range(lsm, result);

	say_verbose("%s: coalesced ranges %s",
		    vy_lsm_name(lsm), vy_range_str(result));
	return true;

fail_commit:
	vy_range_delete(result);
fail_range:
	diag_log();
	say_error("%s: failed to coalesce range %s",
		  vy_lsm_name(lsm), vy_range_str(range));
	return false;
}

void
vy_lsm_force_compaction(struct vy_lsm *lsm)
{
	struct vy_range *range;
	struct vy_range_tree_iterator it;

	vy_range_tree_ifirst(&lsm->range_tree, &it);
	while ((range = vy_range_tree_inext(&it)) != NULL) {
		range->needs_compaction = true;
		vy_lsm_update_range(lsm, range, NULL, NULL);
	}
}
