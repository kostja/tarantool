#pragma once
/*
 * Copyright 2026, Tarantool AUTHORS, please see AUTHORS file.
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
 *    copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials
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
#include <stddef.h>
#include <stdint.h>
#include <small/ibuf.h>
#include "vy_stat.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_dict_last;

struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

enum {
	/**
	 * Default training period: train on every Nth dump.
	 * Adapts at runtime (see vy_dict_last_adjust_period).
	 */
	VY_DICT_TRAIN_PERIOD_DEFAULT = 10,
	/** Minimum training period (most aggressive). */
	VY_DICT_TRAIN_PERIOD_MIN = 4,
	/** Maximum training period (most conservative). */
	VY_DICT_TRAIN_PERIOD_MAX = 256,
	/**
	 * Minimum compression gain (in percent) required to accept
	 * a trained dictionary. Tweaking this may increase noise in
	 * dict acceptance, so it should be kept fairly high.
	 */
	VY_DICT_MIN_GAIN_PCT = 5,
};

/* ---------- vy_dict ---------- */

/**
 * A ZSTD dictionary used for compressing and decompressing Vinyl
 * run pages.
 *
 * Dictionaries are shared between runs: multiple runs may reference
 * the same dictionary. Reference counting tracks the lifetime;
 * the dictionary is freed automatically when the last reference
 * is dropped (see on_drop below). Dictionary data is persisted
 * in vylog so that it survives restart.
 *
 * The cdict/ddict handles are created once from the raw data and
 * cached for the lifetime of the dictionary. cdict is used by the
 * dump/compaction worker thread for compression; ddict is used by
 * reader threads (cbus page read callbacks) for decompression.
 * Both are thread-safe for concurrent read-only use.
 */
struct vy_dict {
	/**
	 * Dictionary id, persisted in vylog. Equals the ID of the
	 * first run that committed this dictionary, or 0 if the
	 * dictionary hasn't been committed to vylog yet.
	 */
	int64_t id;
	/** Reference count (held by runs and dict_last). */
	uint32_t refs;
	/** Raw dictionary data size in bytes. */
	size_t size;
	/** Raw dictionary data (owned). */
	char *data;
	/** Precomputed ZSTD compression dictionary. */
	struct ZSTD_CDict_s *cdict;
	/** Precomputed ZSTD decompression dictionary. */
	struct ZSTD_DDict_s *ddict;
	/**
	 * Optional callback invoked by vy_dict_unref() just
	 * before the last reference is dropped (refs == 1).
	 * Used to remove the dict from the global hash and
	 * update stats without coupling vy_dict to vy_lsm_env.
	 * Set by vy_lsm_add_dict(). NULL for dicts not yet
	 * registered in the hash (id == 0).
	 */
	void (*on_drop)(struct vy_dict *dict, void *ctx);
	/** Opaque context passed to on_drop. */
	void *on_drop_ctx;
};

/**
 * Allocate a vy_dict and precompute cdict/ddict from raw data.
 * Returns NULL on OOM or ZSTD error (diag is set).
 */
struct vy_dict *
vy_dict_new(int64_t id, const void *data, size_t size, int compression_level);

void
vy_dict_delete(struct vy_dict *dict);

/** Increment the reference count. */
void
vy_dict_ref(struct vy_dict *dict);

/** Decrement the reference count; free when it drops to zero. */
void
vy_dict_unref(struct vy_dict *dict);

/* ---------- vy_dict_sample ---------- */

/**
 * Per-task dictionary training context, embedded in struct vy_task.
 *
 * Collects raw tuple samples during dump/compaction, then trains
 * a ZSTD dictionary from them and evaluates its effectiveness.
 *
 * Lifecycle (all on the worker thread unless noted):
 *
 *  1. calloc zero-init as part of vy_task (tx thread)
 *  2. vy_dict_sample_init()    -- allocate buffers, enable/disable
 *                                 (tx thread, in task constructor)
 *  3. vy_dict_sample_add()     -- collect samples (worker thread)
 *  4. vy_dict_sample_finish()  -- train + evaluate (worker thread)
 *  5. task completion handler reads back the result:
 *     - has_result: accepted, transfer to lsm->dict_last
 *     - no result: rejected or not attempted
 *     (tx thread)
 *  6. vy_dict_sample_destroy() -- free all buffers (tx thread)
 *
 * The sample buffers (sample_buf, sample_sizes, zbuf) are
 * allocated lazily and freed in destroy. They are only accessed
 * by the worker thread between init and finish.
 *
 * The output fields (data, size, stat_delta, index_stat) are
 * written by the worker thread in finish and read by the tx
 * thread in the completion handler. No concurrent access occurs
 * because the task acts as a synchronization barrier.
 */
struct vy_dict_sample {
	/**
	 * Backing buffer for all sample allocations.
	 * Allocated from the slab cache in vy_dict_sample_init.
	 */
	struct ibuf buf;
	/** Sample buffer: concatenated raw tuple bodies. */
	char *sample_buf;
	/** Per-sample sizes (parallel array, one entry per tuple). */
	size_t *sample_sizes;
	/** Scratch buffer for trial compression during evaluation. */
	char *zbuf;
	/** Training statistics delta, merged into global stats. */
	struct vy_dict_stat stat_delta;
	/** Per-index evaluation stats (gain, rate, CPU cost). */
	struct vy_dict_index_stat index_stat;
	/**
	 * Dictionary used by the new run, against which the newly
	 * trained dictionary is evaluated. NULL if no dictionary
	 * was assigned to the run. The run holds a ref, so the
	 * pointer stays valid for the lifetime of the sample.
	 */
	const struct vy_dict *base_dict;
	/** Number of samples collected so far. */
	uint32_t sample_count;
	/** Total bytes across all samples. */
	size_t sample_size;
	/**
	 * Dictionary training buffer and output. Pre-allocated
	 * at VY_DICT_TARGET_SIZE. Training succeeded if size > 0
	 * after vy_dict_sample_finish.
	 */
	char *data;
	/** Trained dictionary size (0 if not trained or rejected). */
	size_t size;
	/** Whether training is active for this task. */
	bool enabled;
};

/**
 * Initialize the sample for the upcoming dump/compaction task.
 *
 * Training is enabled only when @a train_period > 0 (dictionary
 * compression is on) and @a dump_count falls on the period
 * boundary. Otherwise the training is silently skipped (counted
 * as throttled if the period is positive).
 *
 * Must be called on the tx thread before the task is submitted.
 */
void
vy_dict_sample_init(struct vy_dict_sample *sample,
		    int64_t dump_count,
		    int64_t train_period,
		    const struct vy_dict *base_dict);

/** Free all buffers and training output. */
void
vy_dict_sample_destroy(struct vy_dict_sample *sample);

/**
 * Feed a tuple into the training sample. Called by the run writer
 * for each statement during dump/compaction (worker thread).
 *
 * We train on raw tuple msgpack rather than the full xrow encoding
 * because the xrow headers are repetitive within each page and
 * ZSTD captures them via its normal within-frame matching.  The
 * dictionary budget is better spent on tuple-body patterns that
 * are consistent across pages but too sparse within a single page
 * for ZSTD to discover on its own.
 *
 * Silently stops collecting once the sample budget is exhausted.
 */
void
vy_dict_sample_add(struct vy_dict_sample *sample,
		   const char *data, size_t size);

/**
 * Train a dictionary from collected samples and evaluate it.
 *
 * If sample->base_dict is not NULL, the new dictionary must
 * compress the sample at least VY_DICT_MIN_GAIN_PCT better than
 * the base to be accepted. If base_dict is NULL, the comparison
 * is against plain ZSTD (no dictionary).
 *
 * On success, the sample holds a trained dictionary (check with
 * sample->size > 0). On rejection, no result is produced.
 *
 * Called on the worker thread after the run is fully written.
 */
void
vy_dict_sample_finish(struct vy_dict_sample *sample,
		      int compression_level);

/* ---------- vy_dict_last ---------- */

/**
 * Per-LSM dictionary state: training policy and the currently
 * active dictionary.
 *
 * When a trained dictionary is accepted from a completed task,
 * a vy_dict object is created immediately and installed as the
 * active dict. A dict with id == 0 hasn't been committed to
 * vylog yet; its data will be included in the next
 * VY_LOG_PREPARE_RUN record. Once committed, id is set to the
 * originating run's ID.
 *
 * Embedded in struct vy_lsm. Only accessed from the tx thread.
 */
struct vy_dict_last {
	/**
	 * Adaptive training period, in dumps (0 = disabled).
	 * Halved when a trained dictionary is accepted, doubled
	 * when rejected, clamped to [MIN, MAX].
	 */
	int64_t train_period;
	/**
	 * Currently active dictionary (ref-counted). Assigned to
	 * new runs when no better trained dictionary is available.
	 * Set during recovery from the most recent run's dict, and
	 * updated when a newly trained dictionary is accepted.
	 *
	 * If dict != NULL && dict->id == 0, the dictionary has
	 * not yet been committed to vylog.
	 */
	struct vy_dict *dict;
};

/** Zero-initialize the dictionary state. */
void
vy_dict_last_create(struct vy_dict_last *last, bool compression_dict);

/** Release the active dict (if any) and reset the state. */
void
vy_dict_last_destroy(struct vy_dict_last *last);

/**
 * Adjust the adaptive training period based on the outcome of
 * the completed task.
 *
 * Accepted results halve the period; rejected results double it,
 * clamped to [VY_DICT_TRAIN_PERIOD_MIN, VY_DICT_TRAIN_PERIOD_MAX].
 */
void
vy_dict_last_adjust_period(struct vy_dict_last *last,
			   const struct vy_dict_sample *sample);

/**
 * Reset the training period to VY_DICT_TRAIN_PERIOD_DEFAULT,
 * restarting the discovery loop.  No-op if training is disabled
 * (train_period == 0).
 */
static inline void
vy_dict_last_reset_period(struct vy_dict_last *last)
{
	if (last->train_period > 0)
		last->train_period = VY_DICT_TRAIN_PERIOD_DEFAULT;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */
