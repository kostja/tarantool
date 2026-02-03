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
#include "vy_dict.h"

#include <string.h>
#include <zstd.h>
#include <zdict.h>

#include "clock.h"
#include "diag.h"
#include "errcode.h"
#include "fiber.h"
#include "say.h"

enum {
	VY_DICT_SAMPLE_MAX = 2 * 1024 * 1024,
	VY_DICT_SAMPLE_MIN = 8 * 1024,
	VY_DICT_TARGET_SIZE = 64 * 1024,
	VY_DICT_MIN_SAMPLES = 16,
	/** Maximum number of individual samples (tuples). */
	VY_DICT_SAMPLE_MAX_COUNT = 64 * 1024,
};

/* ---------- vy_dict ---------- */

struct vy_dict *
vy_dict_new(int64_t id, const void *data, size_t size,
	    int compression_level)
{
	struct vy_dict *dict = calloc(1, sizeof(*dict));
	if (dict == NULL) {
		diag_set(OutOfMemory, sizeof(*dict), "calloc", "vy_dict");
		return NULL;
	}
	dict->id = id;
	dict->size = size;
	dict->data = malloc(size);
	if (dict->data == NULL) {
		diag_set(OutOfMemory, size, "malloc", "vy_dict data");
		goto error;
	}
	memcpy(dict->data, data, size);
	dict->cdict = ZSTD_createCDict(dict->data, dict->size,
				       compression_level);
	if (dict->cdict == NULL) {
		diag_set(ClientError, ER_COMPRESSION,
			 "failed to create dictionary");
		goto error;
	}
	dict->ddict = ZSTD_createDDict(dict->data, dict->size);
	if (dict->ddict == NULL) {
		diag_set(ClientError, ER_COMPRESSION,
			 "failed to create dictionary");
		goto error;
	}
	return dict;
error:
	vy_dict_delete(dict);
	return NULL;
}

void
vy_dict_delete(struct vy_dict *dict)
{
	ZSTD_freeCDict(dict->cdict);
	ZSTD_freeDDict(dict->ddict);
	free(dict->data);
	free(dict);
}

void
vy_dict_ref(struct vy_dict *dict)
{
	assert(dict->refs < UINT32_MAX);
	dict->refs++;
}

void
vy_dict_unref(struct vy_dict *dict)
{
	assert(dict->refs > 0);
	if (dict->refs == 1 && dict->on_drop != NULL)
		dict->on_drop(dict, dict->on_drop_ctx);
	if (--dict->refs == 0)
		vy_dict_delete(dict);
}

/* ---------- vy_dict_sample (static helpers) ---------- */

/**
 * Trial-compress @a data using a dictionary (or without one if
 * @a dict is NULL). Uses sample->zbuf as the destination buffer.
 * Returns compressed size or a ZSTD error code.
 */
static size_t
vy_dict_compress_size(struct vy_dict_sample *sample,
		      const void *dict, size_t dict_size,
		      const void *data, size_t data_size,
		      int compression_level, int64_t *cpu_time_ns)
{
	size_t bound = ZSTD_compressBound(data_size);
	ZSTD_CCtx *cctx = ZSTD_createCCtx();
	if (cctx == NULL)
		return ZSTD_CONTENTSIZE_ERROR;
	int64_t start = 0;
	if (cpu_time_ns != NULL)
		start = clock_thread64();
	size_t rc;
	if (dict != NULL) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		rc = ZSTD_compress_usingDict(cctx, sample->zbuf, bound,
					     data, data_size,
					     dict, dict_size,
					     compression_level);
#pragma GCC diagnostic pop
	} else {
		rc = ZSTD_compressCCtx(cctx, sample->zbuf, bound,
				       data, data_size,
				       compression_level);
	}
	if (cpu_time_ns != NULL)
		*cpu_time_ns = clock_thread64() - start;
	ZSTD_freeCCtx(cctx);
	return rc;
}

/* ---------- vy_dict_sample ---------- */

void
vy_dict_sample_init(struct vy_dict_sample *sample,
		    int64_t dump_count, int64_t train_period,
		    const struct vy_dict *base_dict)
{
	sample->base_dict = base_dict;
	if (train_period <= 0) {
		sample->enabled = false;
		return;
	}
	if (dump_count % train_period != 0) {
		sample->enabled = false;
		sample->stat_delta.train_throttled++;
		return;
	}
	size_t zbuf_size = ZSTD_compressBound(VY_DICT_SAMPLE_MAX);
	size_t sizes_size = VY_DICT_SAMPLE_MAX_COUNT *
			    sizeof(*sample->sample_sizes);
	size_t total = VY_DICT_SAMPLE_MAX + sizes_size + zbuf_size +
		       VY_DICT_TARGET_SIZE;
	ibuf_create(&sample->buf, cord_slab_cache(), total);
	sample->sample_buf = ibuf_alloc(&sample->buf, VY_DICT_SAMPLE_MAX);
	if (sample->sample_buf == NULL) {
		diag_set(OutOfMemory, total, "ibuf", "dict sample");
		sample->enabled = false;
		return;
	}
	sample->sample_sizes = ibuf_alloc(&sample->buf, sizes_size);
	sample->zbuf = ibuf_alloc(&sample->buf, zbuf_size);
	sample->data = ibuf_alloc(&sample->buf, VY_DICT_TARGET_SIZE);
	assert(sample->sample_sizes != NULL && sample->zbuf != NULL);
	assert(sample->data != NULL);
	sample->size = 0;
	sample->sample_count = 0;
	sample->sample_size = 0;
	sample->enabled = true;
}

void
vy_dict_sample_destroy(struct vy_dict_sample *sample)
{
	ibuf_destroy(&sample->buf);
	sample->sample_buf = NULL;
	sample->sample_sizes = NULL;
	sample->zbuf = NULL;
	sample->data = NULL;
	sample->size = 0;
	sample->enabled = false;
}

void
vy_dict_sample_add(struct vy_dict_sample *sample,
		   const char *data, size_t size)
{
	if (!sample->enabled)
		return;
	if (size == 0)
		return;
	if (sample->sample_size + size > VY_DICT_SAMPLE_MAX)
		return;
	if (sample->sample_count >= VY_DICT_SAMPLE_MAX_COUNT)
		return;
	memcpy(sample->sample_buf + sample->sample_size, data, size);
	sample->sample_sizes[sample->sample_count] = size;
	sample->sample_count++;
	sample->sample_size += size;
}

void
vy_dict_sample_finish(struct vy_dict_sample *sample,
		      int compression_level)
{
	if (!sample->enabled)
		return;
	if (sample->sample_count < VY_DICT_MIN_SAMPLES ||
	    sample->sample_size < VY_DICT_SAMPLE_MIN) {
		return;
	}
	sample->stat_delta.train_attempted++;
	void *dict = sample->data;
	const void *samples = sample->sample_buf;
	const size_t *sizes = sample->sample_sizes;
	size_t dict_size = ZDICT_trainFromBuffer(
		dict, VY_DICT_TARGET_SIZE, samples, sizes,
		sample->sample_count);
	if (ZDICT_isError(dict_size)) {
		sample->stat_delta.train_failed++;
		return;
	}
	int64_t with_dict_cpu_ns = 0;
	int64_t no_dict_cpu_ns = 0;
	size_t new_size = vy_dict_compress_size(
		sample, dict, dict_size, samples, sample->sample_size,
		compression_level, &with_dict_cpu_ns);
	if (ZSTD_isError(new_size)) {
		sample->stat_delta.train_failed++;
		return;
	}
	size_t no_dict_size = vy_dict_compress_size(
		sample, NULL, 0, samples, sample->sample_size,
		compression_level, &no_dict_cpu_ns);
	if (ZSTD_isError(no_dict_size)) {
		sample->stat_delta.train_failed++;
		return;
	}
	double dict_gain = 1.0 - (double)new_size / (double)no_dict_size;
	double dict_rate = (double)new_size / (double)sample->sample_size;
	double no_dict_rate = (double)no_dict_size /
			      (double)sample->sample_size;
	sample->index_stat.sample_size = sample->sample_size;
	sample->index_stat.dict_gain = dict_gain;
	sample->index_stat.dict_rate = dict_rate;
	sample->index_stat.dict_cpu_ns = with_dict_cpu_ns;
	sample->index_stat.no_dict_rate = no_dict_rate;
	sample->index_stat.no_dict_cpu_ns = no_dict_cpu_ns;
	if (sample->base_dict != NULL) {
		size_t old_size = vy_dict_compress_size(
			sample, sample->base_dict->data,
			sample->base_dict->size,
			samples, sample->sample_size,
			compression_level, NULL);
		if (ZSTD_isError(old_size)) {
			sample->stat_delta.train_failed++;
			return;
		}
		size_t gain = (old_size > new_size) ?
			(old_size - new_size) * 100 / old_size : 0;
		if (gain < VY_DICT_MIN_GAIN_PCT) {
			sample->stat_delta.train_rejected++;
			return;
		}
	} else if (dict_gain * 100 < VY_DICT_MIN_GAIN_PCT) {
		/* First training: compare against no-dict baseline. */
		sample->stat_delta.train_rejected++;
		return;
	}
	sample->size = dict_size;
	sample->stat_delta.train_accepted++;
}

/* ---------- vy_dict_last ---------- */

void
vy_dict_last_create(struct vy_dict_last *last, bool compression_dict)
{
	memset(last, 0, sizeof(*last));
	if (compression_dict)
		last->train_period = VY_DICT_TRAIN_PERIOD_DEFAULT;
}

void
vy_dict_last_destroy(struct vy_dict_last *last)
{
	if (last->dict != NULL) {
		vy_dict_unref(last->dict);
		last->dict = NULL;
	}
}

void
vy_dict_last_adjust_period(struct vy_dict_last *last,
			   const struct vy_dict_sample *sample)
{
	int64_t used_id = sample->base_dict != NULL ?
			  sample->base_dict->id : 0;
	if (used_id > 0 || sample->size > 0) {
		say_verbose("task completed with dict "
			    "(used_id=%lld, has_result=%d)",
			    (long long)used_id,
			    (int)(sample->size > 0));
	}
	const struct vy_dict_stat *sd = &sample->stat_delta;
	if (sd->train_accepted > 0) {
		last->train_period /= 2;
		if (last->train_period < VY_DICT_TRAIN_PERIOD_MIN)
			last->train_period = VY_DICT_TRAIN_PERIOD_MIN;
	} else if (sd->train_rejected > 0) {
		last->train_period *= 2;
		if (last->train_period > VY_DICT_TRAIN_PERIOD_MAX)
			last->train_period = VY_DICT_TRAIN_PERIOD_MAX;
	}
}
