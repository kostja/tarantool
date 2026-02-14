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
#include "vy_run.h"

#include <zstd.h>

#include "fiber.h"
#include "fiber_cond.h"
#include "fio.h"
#include "cbus.h"
#include "memory.h"
#include "coio_task.h"
#include "mp_util.h"
#include "replication.h"
#include "xlog.h"
#include "binaryfusefilter.h"
#include "salad/minhash.h"
#include "errinj.h"
#include "xrow.h"
#include "vy_history.h"

static const uint64_t vy_page_info_key_map = (1 << VY_PAGE_INFO_OFFSET) |
					     (1 << VY_PAGE_INFO_SIZE) |
					     (1 << VY_PAGE_INFO_UNPACKED_SIZE) |
					     (1 << VY_PAGE_INFO_ROW_COUNT) |
					     (1 << VY_PAGE_INFO_MIN_KEY) |
					     (1 << VY_PAGE_INFO_ROW_INDEX_OFFSET);

static const uint64_t vy_run_info_key_map = (1 << VY_RUN_INFO_MIN_KEY) |
					    (1 << VY_RUN_INFO_MAX_KEY) |
					    (1 << VY_RUN_INFO_MIN_LSN) |
					    (1 << VY_RUN_INFO_MAX_LSN) |
					    (1 << VY_RUN_INFO_PAGE_COUNT);

/** xlog meta type for .run files */
#define XLOG_META_TYPE_RUN "RUN"

/** xlog meta type for .index2 files (partitioned page index) */
#define XLOG_META_TYPE_INDEX2 "INDEX2"

const char *vy_file_suffix[] = {
	"index",			/* VY_FILE_INDEX */
	"index" inprogress_suffix, 	/* VY_FILE_INDEX_INPROGRESS */
	"index2",			/* VY_FILE_INDEX2 */
	"index2" inprogress_suffix,	/* VY_FILE_INDEX2_INPROGRESS */
	"run",				/* VY_FILE_RUN */
	"run" inprogress_suffix, 	/* VY_FILE_RUN_INPROGRESS */
};

/* sync run and index files very 16 MB */
#define VY_RUN_SYNC_INTERVAL (1 << 24)

/**
 * We read runs in background threads so as not to stall tx.
 * This structure represents such a thread.
 */
struct vy_run_reader {
	/** Thread that processes read requests. */
	struct cord cord;
	/** Pipe from tx to the reader thread. */
	struct cpipe reader_pipe;
	/** Pipe from the reader thread to tx. */
	struct cpipe tx_pipe;
};

/** Cbus task for vinyl page read. */
struct vy_page_read_task {
	/** parent */
	struct cbus_call_msg base;
	/**
	 * Copy of vinyl page metadata.  Stored by value (not
	 * pointer) because the source lives in the index cache
	 * and can be evicted while the coio read is in progress.
	 */
	struct vy_page_info page_info;
	/** vy_run with fd - ref. counted */
	struct vy_run *run;
	/** key to lookup within the page */
	struct vy_entry key;
	/** iterator type (needed for for key lookup) */
	enum iterator_type iterator_type;
	/** key definition (needed for key lookup) */
	struct key_def *cmp_def;
	/** disk format (needed for key lookup) */
	struct tuple_format *format;
	/** [out] position of the key in the page */
	uint32_t pos_in_page;
	/** [out] true if key was found in the page */
	bool equal_found;
	/** [out] resulting vinyl page */
	struct vy_page *page;
};

/** Cbus task for vinyl index block read (demand-loading from .index2). */
struct vy_index_block_read_task {
	/** parent */
	struct cbus_call_msg base;
	/** Run to read from (has index_fd). */
	struct vy_run *run;
	/** Block number in the run's block directory. */
	uint32_t block_no;
	/**
	 * [out] Decoded page_info entries.
	 * Allocated by the callback; ownership is transferred
	 * to the caller (and eventually to the index cache).
	 */
	struct vy_page_info *pages;
	/** [out] Number of decoded pages. */
	uint32_t page_count;
	/** [out] Memory used by the decoded pages (for cache accounting). */
	size_t mem_used;
	/** [out] Binary fuse8 filter (if present in the block). */
	binary_fuse8_t filter;
	bool has_filter;
	/** [out] MinHash sketch (if present in the block). */
	struct minhash sketch;
	bool has_sketch;
	/** Key definition (needed for decoding min_key hints). */
	struct key_def *cmp_def;
};

/** Destructor for env->zdctx_key thread-local variable */
static void
vy_free_zdctx(void *arg)
{
	assert(arg != NULL);
	ZSTD_freeDStream(arg);
}

/** Run reader thread function. */
static int
vy_run_reader_f(va_list ap)
{
	struct vy_run_reader *reader = va_arg(ap, struct vy_run_reader *);
	struct cbus_endpoint endpoint;

	cpipe_create(&reader->tx_pipe, "tx_prio");
	cbus_endpoint_create(&endpoint, cord_name(cord()),
			     fiber_schedule_cb, fiber());
	cbus_loop(&endpoint);
	cbus_endpoint_destroy(&endpoint, cbus_process);
	cpipe_destroy(&reader->tx_pipe);
	return 0;
}

/** Start run reader threads. */
static void
vy_run_env_start_readers(struct vy_run_env *env)
{
	assert(env->reader_pool == NULL);
	assert(env->reader_pool_size > 0);

	env->reader_pool = calloc(env->reader_pool_size,
				  sizeof(*env->reader_pool));
	if (env->reader_pool == NULL)
		panic("failed to allocate vinyl reader thread pool");

	for (int i = 0; i < env->reader_pool_size; i++) {
		struct vy_run_reader *reader = &env->reader_pool[i];
		char name[FIBER_NAME_MAX];

		snprintf(name, sizeof(name), "vinyl.reader.%d", i);
		if (cord_costart(&reader->cord, name,
				 vy_run_reader_f, reader) != 0) {
			diag_log();
			panic("failed to start vinyl reader thread");
		}
		cpipe_create(&reader->reader_pipe, name);
	}
	env->next_reader = 0;
}

/** Join run reader threads. */
static void
vy_run_env_stop_readers(struct vy_run_env *env)
{
	for (int i = 0; i < env->reader_pool_size; i++) {
		struct vy_run_reader *reader = &env->reader_pool[i];
		cord_cancel_and_join(&reader->cord);
	}
	free(env->reader_pool);
}

/**
 * Initialize vinyl run environment
 */
void
vy_run_env_create(struct vy_run_env *env, struct tuple_format *key_format,
		  int read_threads)
{
	memset(env, 0, sizeof(*env));
	env->key_format = key_format;
	tuple_format_ref(key_format);
	env->reader_pool_size = read_threads;
	tt_pthread_key_create(&env->zdctx_key, vy_free_zdctx);
	mempool_create(&env->read_task_pool, cord_slab_cache(),
		       sizeof(struct vy_page_read_task));
	vy_index_cache_create(&env->index_cache, VY_INDEX_CACHE_DEFAULT_SIZE);
	/* Use cord-local seed, immune to background thread races. */
	env->seed = rand_r(&cord()->seed);
	env->initial_join = false;
}

/**
 * Destroy vinyl run environment
 */
void
vy_run_env_destroy(struct vy_run_env *env)
{
	if (env->reader_pool != NULL)
		vy_run_env_stop_readers(env);
	vy_index_cache_destroy(&env->index_cache);
	mempool_destroy(&env->read_task_pool);
	tt_pthread_key_delete(env->zdctx_key);
	tuple_format_unref(env->key_format);
}

/**
 * Enable coio reads for a vinyl run environment.
 */
void
vy_run_env_enable_coio(struct vy_run_env *env)
{
	if (env->reader_pool != NULL)
		return; /* already enabled */
	vy_run_env_start_readers(env);
}

/**
 * Execute a task on behalf of a reader thread.
 */
static int
vy_run_env_coio_call(struct vy_run_env *env, struct cbus_call_msg *msg,
		     cbus_call_f func)
{
	/* Optimization: use blocking I/O during WAL recovery. */
	if (env->reader_pool == NULL)
		return func(msg);

	/* Pick a reader thread. */
	struct vy_run_reader *reader;
	reader = &env->reader_pool[env->next_reader++];
	env->next_reader %= env->reader_pool_size;

	/* Post the task to the reader thread. */
	if (cbus_call(&reader->reader_pipe, &reader->tx_pipe, msg, func) != 0)
		return -1;

	if (fiber_is_cancelled()) {
		diag_set(FiberIsCancelled);
		return -1;
	}
	return 0;
}

/**
 * Initialize page info struct
 */
static void
vy_page_info_create(struct vy_page_info *page_info, uint64_t offset,
		    const char *min_key, struct key_def *cmp_def)
{
	memset(page_info, 0, sizeof(*page_info));
	page_info->offset = offset;
	page_info->unpacked_size = 0;
	page_info->min_key = mp_dup(min_key);
	uint32_t part_count = mp_decode_array(&min_key);
	page_info->min_key_hint = key_hint(min_key, part_count, cmp_def);
}

/**
 * Destroy page info struct
 */
static void
vy_page_info_destroy(struct vy_page_info *page_info)
{
	if (page_info->min_key != NULL)
		free(page_info->min_key);
}

struct vy_run *
vy_run_new(struct vy_run_env *env, int64_t id)
{
	struct vy_run *run = calloc(1, sizeof(struct vy_run));
	if (unlikely(run == NULL)) {
		diag_set(OutOfMemory, sizeof(struct vy_run), "malloc",
			 "struct vy_run");
		return NULL;
	}
	run->env = env;
	run->id = id;
	run->dump_lsn = -1;
	run->fd = -1;
	run->index_fd = -1;
	run->refs = 1;
	rlist_create(&run->in_lsm);
	rlist_create(&run->in_unused);
	return run;
}

static void
vy_run_clear(struct vy_run *run)
{
	if (run->page_info != NULL) {
		uint32_t page_no;
		for (page_no = 0; page_no < run->info.page_count; ++page_no)
			vy_page_info_destroy(run->page_info + page_no);
		free(run->page_info);
	}
	run->page_info = NULL;
	run->page_index_size = 0;
	run->info.page_count = 0;
	if (run->block_dir != NULL) {
		for (uint32_t i = 0; i < run->block_count; i++) {
			free(run->block_dir[i].boundary_key);
			if (run->block_dir[i].has_filter)
				binary_fuse8_free(&run->block_dir[i].filter);
		}
		free(run->block_dir);
	}
	run->block_dir = NULL;
	run->block_count = 0;
	free(run->info.min_key);
	run->info.min_key = NULL;
	free(run->info.max_key);
	run->info.max_key = NULL;
}

void
vy_run_delete(struct vy_run *run)
{
	assert(run->refs == 0);
	/*
	 * No need to invalidate index cache entries for this run:
	 * they will naturally gravitate towards the end of the
	 * LRU chain and get evicted when memory is needed.
	 */
	if (run->fd >= 0 && close(run->fd) < 0)
		say_syserror("close failed");
	if (run->index_fd >= 0 && close(run->index_fd) < 0)
		say_syserror("close failed");
	vy_run_clear(run);
	TRASH(run);
	free(run);
}

/* Forward declaration for demand-load from .index2. */
static struct vy_index_cache_entry *
vy_run_get_index_block(struct vy_run *run, uint32_t block_no,
		       struct key_def *cmp_def,
		       struct vy_run_iterator_stat *stat);

uint32_t
vy_block_dir_find_block(struct vy_run *run, struct vy_entry key,
			struct key_def *cmp_def, enum iterator_type itype)
{
	bool is_lower_bound = itype == ITER_LT || itype == ITER_GE;

	assert(run->block_dir != NULL);
	assert(run->block_count > 0);

	int32_t brange[2] = { -1, (int32_t)run->block_count };
	do {
		int32_t mid = brange[0] + (brange[1] - brange[0]) / 2;
		struct vy_index_block_dir *d = &run->block_dir[mid];
		int cmp = vy_entry_compare_with_raw_key(key,
				d->boundary_key, d->boundary_key_hint,
				cmp_def);
		if (is_lower_bound)
			brange[cmp <= 0] = mid;
		else
			brange[cmp < 0] = mid;
	} while (brange[1] - brange[0] > 1);

	if (brange[0] < 0)
		return 0;
	return (uint32_t)brange[0];
}

/**
 * Two-level search using block directory + index cache.
 *
 * Level 1: binary search on block directory boundary keys to find
 * the candidate block.
 * Level 2: demand-load the block from the index cache and binary
 * search within its page_info entries.
 *
 * @retval  0 Success, *result is set to the target page number.
 * @retval -1 Error (disk I/O failure during demand-load).
 */
static int
vy_page_index_find_page_impl(struct vy_run *run, struct vy_entry key,
			     struct key_def *cmp_def, enum iterator_type itype,
			     bool *equal_key, uint32_t *result)
{
	int dir = iterator_direction(itype);
	bool is_lower_bound = itype == ITER_LT || itype == ITER_GE;

	assert(run->block_count > 0);
	assert(run->info.page_count > 0);

	/*
	 * Level 1: binary search in block directory.
	 * Same algorithm as the page-level search, applied
	 * to block boundary keys.
	 */
	int32_t brange[2] = { -1, (int32_t)run->block_count };
	do {
		int32_t mid = brange[0] + (brange[1] - brange[0]) / 2;
		struct vy_index_block_dir *d = &run->block_dir[mid];
		int cmp = vy_entry_compare_with_raw_key(key,
				d->boundary_key, d->boundary_key_hint,
				cmp_def);
		if (is_lower_bound)
			brange[cmp <= 0] = mid;
		else
			brange[cmp < 0] = mid;
		*equal_key = *equal_key || cmp == 0;
	} while (brange[1] - brange[0] > 1);

	/*
	 * The target page is in the last block whose boundary key
	 * satisfies the search condition, i.e. brange[0].
	 */
	int32_t block_idx = brange[0];

	if (block_idx < 0) {
		if (dir < 0) {
			/*
			 * Key precedes all pages: no match
			 * for reverse search.
			 */
			*result = run->info.page_count;
			return 0;
		}
		/* Forward: key < all boundary keys, start at block 0. */
		block_idx = 0;
	}

	/*
	 * Level 2: load the block and search within it.
	 */
	struct vy_index_cache_entry *entry =
		vy_run_get_index_block(run, block_idx, cmp_def, NULL);
	if (entry == NULL)
		return -1;

	/*
	 * Binary search within the block's pages.
	 */
	int32_t prange[2] = { -1, (int32_t)entry->page_count };
	do {
		int32_t mid = prange[0] + (prange[1] - prange[0]) / 2;
		struct vy_page_info *info = &entry->pages[mid];
		int cmp = vy_entry_compare_with_raw_key(key,
				info->min_key, info->min_key_hint,
				cmp_def);
		if (is_lower_bound)
			prange[cmp <= 0] = mid;
		else
			prange[cmp < 0] = mid;
		*equal_key = *equal_key || cmp == 0;
	} while (prange[1] - prange[0] > 1);

	/*
	 * Extract the result with local-to-global page number
	 * conversion.
	 */
	if (prange[0] < 0)
		prange[0] = (int32_t)entry->page_count;
	uint32_t local_page = prange[dir > 0];

	uint32_t global_page;
	if (local_page >= entry->page_count) {
		/*
		 * Beyond this block: the answer is the first page
		 * of the next block, or page_count if this is the
		 * last block.
		 */
		if ((uint32_t)(block_idx + 1) < run->block_count)
			global_page = run->block_dir[block_idx + 1].first_page_no;
		else
			global_page = run->info.page_count;
	} else {
		global_page = entry->first_page_no + local_page;
	}

	if (global_page > 0 && dir > 0)
		global_page--;

	*result = global_page;
	return 0;
}

int
vy_page_index_find_page(struct vy_run *run, struct vy_entry key,
			struct key_def *cmp_def, enum iterator_type itype,
			bool *equal_key, uint32_t *result)
{
	if (itype == ITER_EQ)
		itype = ITER_GE; /* One day it'll become obsolete */
	assert(itype == ITER_GE || itype == ITER_GT ||
	       itype == ITER_LE || itype == ITER_LT);
	*equal_key = false;

	assert(run->block_dir != NULL);
	return vy_page_index_find_page_impl(run, key, cmp_def, itype,
					    equal_key, result);
}

struct vy_slice *
vy_slice_new(int64_t id, struct vy_run *run)
{
	struct vy_slice *slice = malloc(sizeof(*slice));
	if (slice == NULL) {
		diag_set(OutOfMemory, sizeof(*slice),
			 "malloc", "struct vy_slice");
		return NULL;
	}
	memset(slice, 0, sizeof(*slice));
	slice->id = id;
	slice->run = run;
	/*
	 * Use a vinyl-local seed to avoid impact of other uses of
	 * rand (potentially non-deterministic) on the compaction
	 * scheduler.
	 */
	slice->seed = rand_r(&run->env->seed);
	vy_run_ref(run);
	run->slice_count++;
	rlist_create(&slice->in_range);
	fiber_cond_create(&slice->pin_cond);
	/*
	 * Slice becomes usable only after range-dependent bounds
	 * are initialized.
	 */
	return slice;
}

void
vy_slice_delete(struct vy_slice *slice)
{
	assert(slice->pin_count == 0);
	assert(slice->run->slice_count > 0);
	slice->run->slice_count--;
	vy_run_unref(slice->run);
	if (slice->begin.stmt != NULL)
		tuple_unref(slice->begin.stmt);
	if (slice->end_bound.stmt != NULL)
		tuple_unref(slice->end_bound.stmt);
	fiber_cond_destroy(&slice->pin_cond);
	TRASH(slice);
	free(slice);
}

int
vy_slice_cut(struct vy_slice *slice, int64_t id, struct vy_entry begin,
	     struct vy_entry end, struct key_def *cmp_def,
	     struct vy_slice **result)
{
	*result = NULL;

	if (begin.stmt != NULL &&
	    vy_bound_cmp(begin, slice->end_bound, cmp_def) > 0)
		return 0; /* no intersection: begin past end_bound */

	if (end.stmt != NULL &&
	    vy_entry_compare(end, slice->begin, cmp_def) <= 0)
		return 0; /* no intersection: end <= slice->begin */

	*result = vy_slice_new(id, slice->run);
	if (*result == NULL)
		return -1; /* OOM */

	return 0;
}

/**
 * Decode page information from xrow.
 *
 * @param[out] page Page information.
 * @param xrow      Xrow to decode.
 * @param cmp_def   Definition of keys stored in the page.
 * @param filename  Filename for error reporting.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
int
vy_page_info_decode(struct vy_page_info *page, const struct xrow_header *xrow,
		    struct key_def *cmp_def, const char *filename)
{
	assert(xrow->type == VY_INDEX_PAGE_INFO);
	const char *pos = xrow->body->iov_base;
	memset(page, 0, sizeof(*page));
	uint64_t key_map = vy_page_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	const char *key_beg;
	uint32_t part_count;
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1ULL << key);
		switch (key) {
		case VY_PAGE_INFO_OFFSET:
			page->offset = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_SIZE:
			page->size = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_ROW_COUNT:
			page->row_count = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_MIN_KEY:
			key_beg = pos;
			mp_next(&pos);
			page->min_key = mp_dup(key_beg);
			part_count = mp_decode_array(&key_beg);
			page->min_key_hint = key_hint(key_beg, part_count,
						      cmp_def);
			break;
		case VY_PAGE_INFO_UNPACKED_SIZE:
			page->unpacked_size = mp_decode_uint(&pos);
			break;
		case VY_PAGE_INFO_ROW_INDEX_OFFSET:
			page->row_index_offset = mp_decode_uint(&pos);
			break;
		default:
			mp_next(&pos); /* unknown key, ignore */
			break;
		}
	}
	if (key_map) {
		enum vy_page_info_key key = bit_ctz_u64(key_map);
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode page info: "
				    "missing mandatory key %s",
				    vy_page_info_key_name(key)));
		return -1;
	}

	return 0;
}

/** Decode statement statistics from @data and advance @data. */
static void
vy_stmt_stat_decode(struct vy_stmt_stat *stat, const char **data)
{
	uint32_t size = mp_decode_map(data);
	for (uint32_t i = 0; i < size; i++) {
		uint64_t key = mp_decode_uint(data);
		uint64_t value = mp_decode_uint(data);
		switch (key) {
		case IPROTO_INSERT:
			stat->inserts = value;
			break;
		case IPROTO_REPLACE:
			stat->replaces = value;
			break;
		case IPROTO_DELETE:
			stat->deletes = value;
			break;
		case IPROTO_UPSERT:
			stat->upserts = value;
			break;
		default:
			break;
		}
	}
}

static enum tuple_bloom_version
iproto_to_tuple_bloom_version(uint32_t key)
{
	switch (key) {
	case VY_RUN_INFO_BLOOM_FILTER_LEGACY:
		return TUPLE_BLOOM_VERSION_V1;
	case VY_RUN_INFO_BLOOM_FILTER:
		return TUPLE_BLOOM_VERSION_V2;
	default:
		unreachable();
	}
	return 0;
}

static uint32_t
tuple_bloom_version_to_iproto(enum tuple_bloom_version version)
{
	switch (version) {
	case TUPLE_BLOOM_VERSION_V1:
		return VY_RUN_INFO_BLOOM_FILTER_LEGACY;
	case TUPLE_BLOOM_VERSION_V2:
		return VY_RUN_INFO_BLOOM_FILTER;
	default:
		unreachable();
	}
	return 0;
}

/**
 * Decode the run metadata from xrow.
 *
 * @param xrow xrow to decode
 * @param[out] run_info the run information
 * @param filename File name for error reporting.
 *
 * @retval  0 success
 * @retval -1 error (check diag)
 */
int
vy_run_info_decode(struct vy_run_info *run_info,
		   const struct xrow_header *xrow,
		   const char *filename)
{
	assert(xrow->type == VY_INDEX_RUN_INFO);
	/* decode run */
	const char *pos = xrow->body->iov_base;
	memset(run_info, 0, sizeof(*run_info));
	uint64_t key_map = vy_run_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	const char *tmp;
	/* decode run values */
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1ULL << key);
		switch (key) {
		case VY_RUN_INFO_MIN_KEY:
			tmp = pos;
			mp_next(&pos);
			run_info->min_key = mp_dup(tmp);
			break;
		case VY_RUN_INFO_MAX_KEY:
			tmp = pos;
			mp_next(&pos);
			run_info->max_key = mp_dup(tmp);
			break;
		case VY_RUN_INFO_MIN_LSN:
			run_info->min_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_MAX_LSN:
			run_info->max_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_PAGE_COUNT:
			run_info->page_count = mp_decode_uint(&pos);
			break;
		case VY_RUN_INFO_BLOOM_FILTER_LEGACY:
		case VY_RUN_INFO_BLOOM_FILTER:
			/*
			 * Legacy per-run bloom filters are no longer
			 * used.  Skip the data but don't fail.
			 */
			mp_next(&pos);
			break;
		case VY_RUN_INFO_STMT_STAT:
			vy_stmt_stat_decode(&run_info->stmt_stat, &pos);
			break;
		default:
			mp_next(&pos); /* unknown key, ignore */
			break;
		}
	}
	if (key_map) {
		enum vy_run_info_key key = bit_ctz_u64(key_map);
		diag_set(ClientError, ER_INVALID_INDEX_FILE, filename,
			 tt_sprintf("Can't decode run info: "
				    "missing mandatory key %s",
				    vy_run_info_key_name(key)));
		return -1;
	}
	return 0;
}

static struct vy_page *
vy_page_new(const struct vy_page_info *page_info)
{
	struct vy_page *page = malloc(sizeof(*page));
	if (page == NULL) {
		diag_set(OutOfMemory, sizeof(*page),
			 "load_page", "page cache");
		return NULL;
	}
	page->unpacked_size = page_info->unpacked_size;
	page->row_count = page_info->row_count;
	page->row_index = calloc(page_info->row_count, sizeof(uint32_t));
	if (page->row_index == NULL) {
		diag_set(OutOfMemory, page_info->row_count * sizeof(uint32_t),
			 "malloc", "page->row_index");
		free(page);
		return NULL;
	}

	page->data = (char *)malloc(page_info->unpacked_size);
	if (page->data == NULL) {
		diag_set(OutOfMemory, page_info->unpacked_size,
			 "malloc", "page->data");
		free(page->row_index);
		free(page);
		return NULL;
	}
	return page;
}

static void
vy_page_delete(struct vy_page *page)
{
	uint32_t *row_index = page->row_index;
	char *data = page->data;
#if !defined(NDEBUG)
	memset(row_index, '#', sizeof(uint32_t) * page->row_count);
	memset(data, '#', page->unpacked_size);
	memset(page, '#', sizeof(*page));
#endif /* !defined(NDEBUG) */
	free(row_index);
	free(data);
	free(page);
}

static int
vy_page_xrow(struct vy_page *page, uint32_t stmt_no,
	     struct xrow_header *xrow)
{
	assert(stmt_no < page->row_count);
	const char *data = page->data + page->row_index[stmt_no];
	const char *data_end = stmt_no + 1 < page->row_count ?
			       page->data + page->row_index[stmt_no + 1] :
			       page->data + page->unpacked_size;
	return xrow_header_decode(xrow, &data, data_end, false);
}

/* {{{ vy_run_iterator vy_run_iterator support functions */

/**
 * Read raw stmt data from the page
 * @param page          Page.
 * @param stmt_no       Statement position in the page.
 * @param cmp_def       Definition of keys stored in the page.
 * @param format        Format for REPLACE/DELETE tuples.
 *
 * @retval not NULL Statement read from page.
 * @retval     NULL Memory error.
 */
static struct vy_entry
vy_page_stmt(struct vy_page *page, uint32_t stmt_no,
	     struct key_def *cmp_def, struct tuple_format *format)
{
	struct xrow_header xrow;
	if (vy_page_xrow(page, stmt_no, &xrow) != 0)
		return vy_entry_none();
	struct vy_entry entry;
	entry.stmt = vy_stmt_decode(&xrow, format);
	if (entry.stmt == NULL)
		return vy_entry_none();
	entry.hint = vy_stmt_hint(entry.stmt, cmp_def);
	return entry;
}

/**
 * Binary search in page
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (set to false otherwise).
 */
static int
vy_page_find_key(struct vy_page *page, struct vy_entry key,
		 struct key_def *cmp_def, struct tuple_format *format,
		 enum iterator_type iterator_type, uint32_t *pos,
		 bool *equal_key)
{
	uint32_t beg = 0;
	uint32_t end = page->row_count;
	*equal_key = false;
	/* for upper bound we change zero comparison result to -1 */
	int zero_cmp = (iterator_type == ITER_GT ||
			iterator_type == ITER_LE ? -1 : 0);
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_entry fnd_key = vy_page_stmt(page, mid, cmp_def,
						       format);
		if (fnd_key.stmt == NULL)
			return -1;
		int cmp = vy_entry_compare(fnd_key, key, cmp_def);
		cmp = cmp ? cmp : zero_cmp;
		*equal_key = *equal_key || cmp == 0;
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
		tuple_unref(fnd_key.stmt);
	}
	*pos = end;
	return 0;
}

/**
 * End iteration and free cached data.
 */
static void
vy_run_iterator_stop(struct vy_run_iterator *itr)
{
	if (itr->curr.stmt != NULL) {
		tuple_unref(itr->curr.stmt);
		itr->curr = vy_entry_none();
	}
	if (itr->curr_page != NULL) {
		vy_page_delete(itr->curr_page);
		if (itr->prev_page != NULL)
			vy_page_delete(itr->prev_page);
		itr->curr_page = itr->prev_page = NULL;
	}
}

static int
vy_row_index_decode(uint32_t *row_index, uint32_t row_count,
		    struct xrow_header *xrow)
{
	assert(xrow->type == VY_RUN_ROW_INDEX);
	const char *pos = xrow->body->iov_base;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	uint32_t size = 0;
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		switch (key) {
		case VY_ROW_INDEX_DATA:
			size = mp_decode_binl(&pos);
			break;
		}
	}
	if (size != sizeof(uint32_t) * row_count) {
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 tt_sprintf("Wrong row index size "
				    "(expected %zu, got %u",
				    sizeof(uint32_t) * row_count,
				    (unsigned)size));
		return -1;
	}
	for (uint32_t i = 0; i < row_count; ++i) {
		row_index[i] = mp_load_u32(&pos);
	}
	assert(pos == xrow->body->iov_base + xrow->body->iov_len);
	return 0;
}

/** Return the name of a run data file. */
static inline const char *
vy_run_filename(struct vy_run *run)
{
	char *buf = tt_static_buf();
	vy_run_snprint_filename(buf, TT_STATIC_BUF_LEN, run->id, VY_FILE_RUN);
	return buf;
}

/**
 * Read a page requests from vinyl xlog data file.
 *
 * @retval 0 on success
 * @retval -1 on error, check diag
 */
static int
vy_page_read(struct vy_page *page, const struct vy_page_info *page_info,
	     struct vy_run *run, ZSTD_DStream *zdctx)
{
	/* read xlog tx from xlog file */
	size_t region_svp = region_used(&fiber()->gc);
	char *data = (char *)region_alloc(&fiber()->gc, page_info->size);
	if (data == NULL) {
		diag_set(OutOfMemory, page_info->size, "region gc", "page");
		return -1;
	}
	ssize_t readen = fio_pread(run->fd, data, page_info->size,
				   page_info->offset);
	ERROR_INJECT(ERRINJ_VYRUN_DATA_READ, {
		readen = -1;
		errno = EIO;});
	if (readen < 0) {
		diag_set(SystemError, "failed to read from file");
		goto error;
	}
	if (readen != (ssize_t)page_info->size) {
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 "Unexpected end of file");
		goto error;
	}

	struct errinj *inj = errinj(ERRINJ_VY_READ_PAGE_TIMEOUT, ERRINJ_DOUBLE);
	if (inj != NULL && inj->dparam > 0)
		thread_sleep(inj->dparam);

	ERROR_INJECT_SLEEP(ERRINJ_VY_READ_PAGE_DELAY);

	/* decode xlog tx */
	const char *data_pos = data;
	const char *data_end = data + readen;
	char *rows = page->data;
	char *rows_end = rows + page_info->unpacked_size;
	if (xlog_tx_decode(data, data_end, rows, rows_end, zdctx) != 0)
		goto error;

	struct xrow_header xrow;
	data_pos = page->data + page_info->row_index_offset;
	data_end = page->data + page_info->unpacked_size;
	if (xrow_header_decode(&xrow, &data_pos, data_end, true) == -1)
		goto error;
	if (xrow.type != VY_RUN_ROW_INDEX) {
		diag_set(ClientError, ER_INVALID_RUN_FILE,
			 tt_sprintf("Wrong row index type "
				    "(expected %d, got %u)",
				    VY_RUN_ROW_INDEX, (unsigned)xrow.type));
		goto error;
	}
	if (vy_row_index_decode(page->row_index, page->row_count, &xrow) != 0)
		goto error;
	region_truncate(&fiber()->gc, region_svp);
	ERROR_INJECT(ERRINJ_VY_READ_PAGE, {
		diag_set(ClientError, ER_INJECTION, "vinyl page read");
		return -1;});
	return 0;
error:
	region_truncate(&fiber()->gc, region_svp);
	diag_log();
	say_error("error reading %s@%llu:%u", vy_run_filename(run),
		  (unsigned long long)page_info->offset,
		  (unsigned)page_info->size);
	return -1;
}

/**
 * Get thread local zstd decompression context
 */
static ZSTD_DStream *
vy_env_get_zdctx(struct vy_run_env *env)
{
	ZSTD_DStream *zdctx = tt_pthread_getspecific(env->zdctx_key);
	if (zdctx == NULL) {
		zdctx = ZSTD_createDStream();
		if (zdctx == NULL) {
			diag_set(OutOfMemory, sizeof(zdctx), "malloc",
				 "zstd context");
			return NULL;
		}
		tt_pthread_setspecific(env->zdctx_key, zdctx);
	}
	return zdctx;
}

/**
 * vinyl read task callback
 */
static int
vy_page_read_cb(struct cbus_call_msg *base)
{
	struct vy_page_read_task *task = (struct vy_page_read_task *)base;
	ZSTD_DStream *zdctx = vy_env_get_zdctx(task->run->env);
	if (zdctx == NULL)
		return -1;
	if (vy_page_read(task->page, &task->page_info, task->run, zdctx) != 0)
		return -1;
	if (task->key.stmt != NULL &&
	    vy_page_find_key(task->page, task->key, task->cmp_def,
			     task->format, task->iterator_type,
			     &task->pos_in_page, &task->equal_found) != 0)
		return -1;
	return 0;
}

/**
 * Read an index block from .index2 and return the first xrow.
 *
 * Reads raw data from @a fd at @a offset via pread, decompresses
 * the xlog transaction, and returns the first xrow.  The xrow
 * body points into a region-allocated buffer; the caller must
 * call xlog_tx_cursor_destroy(@a tx_cursor) when done, and
 * region_truncate the fiber gc region.
 *
 * @return 0 on success, -1 on error (diag is set).
 */
static int
vy_index_block_read_xrow(int fd, off_t offset, uint32_t data_size,
			  struct vy_run_env *env,
			  struct xlog_tx_cursor *tx_cursor,
			  struct xrow_header *xrow)
{
	char *buf = (char *)region_alloc(&fiber()->gc, data_size);
	if (buf == NULL) {
		diag_set(OutOfMemory, data_size, "region gc",
			 "index block");
		return -1;
	}
	ssize_t nread = fio_pread(fd, buf, data_size, offset);
	if (nread < 0) {
		diag_set(SystemError, "failed to read index block");
		return -1;
	}
	if (nread != (ssize_t)data_size) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 "Unexpected end of file reading index block");
		return -1;
	}
	ZSTD_DStream *zdctx = vy_env_get_zdctx(env);
	if (zdctx == NULL)
		return -1;

	const char *pos = buf;
	ssize_t rc = xlog_tx_cursor_create(tx_cursor, &pos,
					   buf + data_size, zdctx);
	if (rc != 0) {
		if (rc > 0)
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 "Truncated index block transaction");
		return -1;
	}
	if (xlog_tx_cursor_next_row(tx_cursor, xrow) != 0) {
		xlog_tx_cursor_destroy(tx_cursor);
		return -1;
	}
	return 0;
}

/**
 * Decode a pages array from a msgpack position.
 *
 * @a data must point to a msgpack array of page_info maps.
 * The returned @a pages array is heap-allocated; the caller
 * is responsible for freeing it and each page's min_key.
 *
 * @param[out] mem_out  If non-NULL, receives the total memory
 *                      consumed by the decoded pages and keys.
 * @return 0 on success, -1 on error (diag is set).
 */
int
vy_index_block_decode_pages(const char *data, struct key_def *cmp_def,
			     struct vy_page_info **pages_out,
			     uint32_t *count_out, size_t *mem_out)
{
	uint32_t count = mp_decode_array(&data);
	struct vy_page_info *pages = (struct vy_page_info *)
		calloc(count, sizeof(struct vy_page_info));
	if (pages == NULL) {
		diag_set(OutOfMemory,
			 count * sizeof(struct vy_page_info),
			 "calloc", "vy_page_info");
		return -1;
	}
	size_t mem = count * sizeof(struct vy_page_info);
	for (uint32_t p = 0; p < count; p++) {
		struct xrow_header pi_xrow;
		memset(&pi_xrow, 0, sizeof(pi_xrow));
		pi_xrow.body->iov_base = (void *)data;
		const char *start = data;
		mp_next(&data);
		pi_xrow.body->iov_len = data - start;
		pi_xrow.bodycnt = 1;
		pi_xrow.type = VY_INDEX_PAGE_INFO;
		if (vy_page_info_decode(&pages[p], &pi_xrow,
					cmp_def, "") < 0) {
			for (uint32_t j = 0; j < p; j++) {
				if (pages[j].min_key != NULL)
					free(pages[j].min_key);
			}
			free(pages);
			return -1;
		}
		if (pages[p].min_key != NULL) {
			const char *key_end = pages[p].min_key;
			mp_next(&key_end);
			mem += key_end - pages[p].min_key;
		}
	}
	*pages_out = pages;
	*count_out = count;
	if (mem_out != NULL)
		*mem_out = mem;
	return 0;
}

/**
 * Callback for demand-loading an index block from .index2.
 *
 * Runs on a reader thread.  Reads a single xlog transaction
 * from the .index2 file, decodes the page_info entries, and
 * stores the result in the task's output fields.
 */
static int
vy_index_block_read_cb(struct cbus_call_msg *base)
{
	struct vy_index_block_read_task *task =
		(struct vy_index_block_read_task *)base;
	struct vy_run *run = task->run;
	uint32_t block_no = task->block_no;

	assert(block_no < run->block_count);
	struct vy_index_block_dir *dir = &run->block_dir[block_no];

	size_t region_svp = region_used(&fiber()->gc);

	struct xlog_tx_cursor tx_cursor;
	struct xrow_header xrow;
	if (vy_index_block_read_xrow(run->index_fd, dir->file_offset,
				      dir->data_size, run->env,
				      &tx_cursor, &xrow) != 0)
		goto err;

	if (xrow.type != VY_INDEX_BLOCK) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 "Wrong xrow type in index block");
		xlog_tx_cursor_destroy(&tx_cursor);
		goto err;
	}

	/*
	 * Decode the xrow body.
	 *
	 * New format (v2): the body is a msgpack map with keys
	 *   VY_INDEX_BLOCK_PAGE_INFO, VY_INDEX_BLOCK_FILTER,
	 *   VY_INDEX_BLOCK_SKETCH.
	 *
	 * Old format: the body is a bare msgpack array of page_info
	 * maps (written before PDS support was added).
	 *
	 * We distinguish between the two by peeking at the first
	 * msgpack byte: a map starts with 0xde/0xdf or fixmap
	 * (0x80..0x8f), while an array starts with 0xdc/0xdd or
	 * fixarray (0x90..0x9f).
	 */
	const char *bpos = xrow.body->iov_base;
	const char *pages_data = NULL;
	const char *filter_data = NULL;
	uint32_t filter_size = 0;
	const char *sketch_data = NULL;
	uint32_t sketch_size = 0;

	if (mp_typeof(*bpos) == MP_MAP) {
		/* New map-based format. */
		uint32_t map_size = mp_decode_map(&bpos);
		for (uint32_t i = 0; i < map_size; i++) {
			uint32_t key = mp_decode_uint(&bpos);
			switch (key) {
			case VY_INDEX_BLOCK_PAGE_INFO:
				pages_data = bpos;
				mp_next(&bpos);
				break;
			case VY_INDEX_BLOCK_FILTER:
				filter_size = mp_decode_binl(&bpos);
				filter_data = bpos;
				bpos += filter_size;
				break;
			case VY_INDEX_BLOCK_SKETCH:
				sketch_size = mp_decode_binl(&bpos);
				sketch_data = bpos;
				bpos += sketch_size;
				break;
			default:
				mp_next(&bpos);
				break;
			}
		}
		if (pages_data == NULL) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 "Missing PAGES key in index block");
			xlog_tx_cursor_destroy(&tx_cursor);
			goto err;
		}
	} else {
		/* Old format: body is a bare array. */
		pages_data = bpos;
	}

	/* Decode the pages array. */
	struct vy_page_info *pages;
	uint32_t arr_count;
	size_t mem;
	if (vy_index_block_decode_pages(pages_data, task->cmp_def,
					 &pages, &arr_count, &mem) != 0) {
		xlog_tx_cursor_destroy(&tx_cursor);
		goto err;
	}
	mem += sizeof(struct vy_index_cache_entry);
	if (arr_count != dir->page_count) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 "Index block page count mismatch");
		for (uint32_t j = 0; j < arr_count; j++) {
			if (pages[j].min_key != NULL)
				free(pages[j].min_key);
		}
		free(pages);
		xlog_tx_cursor_destroy(&tx_cursor);
		goto err;
	}

	/* Decode binary fuse8 filter if present. */
	task->has_filter = false;
	if (filter_data != NULL && filter_size > 0) {
		if (binary_fuse8_deserialize(&task->filter, filter_data)) {
			task->has_filter = true;
			mem += binary_fuse8_size_in_bytes(&task->filter);
		}
	}

	/* Decode MinHash sketch if present. */
	task->has_sketch = false;
	if (sketch_data != NULL && sketch_size == MINHASH_SIZE) {
		memcpy(task->sketch.values, sketch_data, MINHASH_SIZE);
		task->has_sketch = true;
	}

	xlog_tx_cursor_destroy(&tx_cursor);
	region_truncate(&fiber()->gc, region_svp);

	task->pages = pages;
	task->page_count = arr_count;
	task->mem_used = mem;
	return 0;
err:
	region_truncate(&fiber()->gc, region_svp);
	return -1;
}

/**
 * Sentinel for tracking an in-flight index block load.
 * Prevents duplicate disk reads when multiple fibers
 * concurrently miss the same cache entry.
 * Stack-allocated by the loading fiber.
 */
struct vy_index_block_loading {
	struct rlist in_list;
	int64_t run_id;
	uint32_t block_no;
	struct fiber_cond cond;
};

/** Free pages and filter loaded by an index block read task. */
static void
vy_index_block_read_task_free(struct vy_index_block_read_task *task)
{
	for (uint32_t i = 0; i < task->page_count; i++) {
		if (task->pages[i].min_key != NULL)
			free(task->pages[i].min_key);
	}
	free(task->pages);
	if (task->has_filter)
		binary_fuse8_free(&task->filter);
}

/**
 * Load an index block for a v2 run, using the index cache.
 *
 * First checks the 2Q cache; on a miss, checks if another fiber
 * is already loading this block (via a sentinel list) and waits
 * for it.  If no one is loading, reads the block from the .index2
 * file via a reader thread and inserts the result into the cache.
 *
 * @param run      The run (must have block_dir != NULL).
 * @param block_no Block number to load.
 * @param cmp_def  Key definition for decoding min_key hints.
 * @return Cache entry with the decoded page_info array,
 *         or NULL on error (diag is set).
 */
static struct vy_index_cache_entry *
vy_run_get_index_block(struct vy_run *run, uint32_t block_no,
		       struct key_def *cmp_def,
		       struct vy_run_iterator_stat *stat)
{
	assert(run->block_dir != NULL);
	assert(block_no < run->block_count);

	struct vy_run_env *env = run->env;
	struct vy_index_cache *cache = &env->index_cache;

	/* Try the cache first. */
	struct vy_index_cache_entry *entry =
		vy_index_cache_get(cache, run->id, block_no);
	if (entry != NULL) {
		cache->stat.hit++;
		if (stat != NULL)
			stat->index_cache_hit++;
		return entry;
	}

	/*
	 * Check if another fiber is already loading this block.
	 * If so, wait for it to finish and use the cached result.
	 * Re-scan the list after wakeup: the first waiter to run
	 * may register as a new loader before we get scheduled.
	 */
	struct vy_index_block_loading *loading;
retry:
	rlist_foreach_entry(loading, &cache->loading, in_list) {
		if (loading->run_id == run->id &&
		    loading->block_no == block_no) {
			fiber_cond_wait(&loading->cond);
			entry = vy_index_cache_get(cache, run->id,
						   block_no);
			if (entry != NULL) {
				cache->stat.hit++;
				if (stat != NULL)
					stat->index_cache_hit++;
				return entry;
			}
			/*
			 * The loading fiber failed.  Re-scan the
			 * sentinel list in case another waiter has
			 * already registered as the new loader.
			 */
			goto retry;
		}
	}

	/*
	 * Count the miss only for actual disk loads (not for
	 * fibers that waited on a sentinel and got a cache hit).
	 */
	cache->stat.miss++;
	if (stat != NULL)
		stat->index_cache_miss++;

	/* Register as the loader for this block. */
	struct vy_index_block_loading self;
	self.run_id = run->id;
	self.block_no = block_no;
	fiber_cond_create(&self.cond);
	rlist_add_entry(&cache->loading, &self, in_list);

	/* Cache miss — demand-load from disk. */
	struct vy_index_block_read_task task;
	memset(&task, 0, sizeof(task));
	task.run = run;
	task.block_no = block_no;
	task.cmp_def = cmp_def;

	entry = NULL;

	if (vy_run_env_coio_call(env, &task.base,
				 vy_index_block_read_cb) != 0)
		goto done;

	ERROR_INJECT(ERRINJ_VY_INDEX_BLOCK_READ, {
		diag_set(ClientError, ER_INJECTION,
			 "vinyl index block read");
		vy_index_block_read_task_free(&task);
		goto done;
	});

	ERROR_INJECT_YIELD(ERRINJ_VY_INDEX_BLOCK_DELAY);

	/*
	 * Re-check the cache: another code path may have
	 * populated it (shouldn't happen with the sentinel,
	 * but check defensively).
	 */
	entry = vy_index_cache_get(cache, run->id, block_no);
	if (entry != NULL) {
		vy_index_block_read_task_free(&task);
		goto done;
	}

	/* Insert into the cache.  Ownership of pages passes to the cache. */
	struct vy_index_block_dir *dir = &run->block_dir[block_no];
	binary_fuse8_t *filter_ptr = task.has_filter ? &task.filter : NULL;
	struct minhash *sketch_ptr = task.has_sketch ? &task.sketch : NULL;
	entry = vy_index_cache_put(cache, run->id, block_no,
				    dir->first_page_no,
				    task.pages, task.page_count,
				    filter_ptr, sketch_ptr,
				    task.mem_used);
	if (entry == NULL) {
		vy_index_block_read_task_free(&task);
		goto done;
	}
done:
	rlist_del_entry(&self, in_list);
	fiber_cond_broadcast(&self.cond);
	fiber_cond_destroy(&self.cond);
	return entry;
}

struct vy_page_info *
vy_run_page_info_v2(struct vy_run *run, uint32_t pos,
		    struct key_def *cmp_def)
{
	assert(run->block_dir != NULL);
	assert(pos < run->info.page_count);

	/*
	 * Find which block contains the given page.
	 * Since blocks have VY_INDEX_BLOCK_SIZE pages (except
	 * possibly the last), simple division works.
	 */
	uint32_t block_no = pos / VY_INDEX_BLOCK_SIZE;
	if (block_no >= run->block_count)
		block_no = run->block_count - 1;

	struct vy_index_cache_entry *entry =
		vy_run_get_index_block(run, block_no, cmp_def, NULL);
	if (entry == NULL)
		return NULL;

	uint32_t local_pos = pos - entry->first_page_no;
	assert(local_pos < entry->page_count);
	return &entry->pages[local_pos];
}

/**
 * Check the per-block binary fuse8 filter to determine if a
 * given key might exist in the run.
 *
 * This is used for ITER_EQ optimization: if the filter says
 * the key is definitely absent, we can skip the entire run.
 *
 * The function finds the block that would contain the key,
 * loads it into the index cache, and checks its fuse filter.
 *
 * @param run     The run to check (must have block_dir != NULL).
 * @param key     The search key.
 * @param key_def Key definition for hashing.
 * @param cmp_def Key definition for comparison.
 *
 * @retval  1 Key is definitely absent (filter hit).
 * @retval  0 Key might be present (filter miss or no filter).
 * @retval -1 Error (diag is set).
 */
static int
vy_run_bloom_check(struct vy_run *run, struct vy_entry key,
		   struct key_def *key_def, struct key_def *cmp_def,
		   bool *checked)
{
	*checked = false;
	if (run->block_dir == NULL || run->block_count == 0)
		return 0;
	/*
	 * The fuse8 filter stores hashes of full keys.  A partial
	 * key (fewer parts than key_def) hashes differently, so
	 * the filter would always report "absent" — a false
	 * negative.  Skip the check for partial keys.
	 */
	if (!vy_stmt_is_full_key(key.stmt, key_def))
		return 0;

	/*
	 * Binary search in block directory to find the block
	 * that would contain this key.
	 */
	int32_t lo = 0, hi = (int32_t)run->block_count;
	while (lo < hi) {
		int32_t mid = lo + (hi - lo) / 2;
		struct vy_index_block_dir *d = &run->block_dir[mid];
		int cmp = vy_entry_compare_with_raw_key(key,
				d->boundary_key, d->boundary_key_hint,
				cmp_def);
		if (cmp < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	/* The key would be in block (lo - 1). */
	if (lo == 0)
		return 0; /* Key precedes all blocks. */
	uint32_t block_no = lo - 1;

	/*
	 * Use the in-memory filter from block_dir.  This avoids
	 * yielding (unlike demand-loading from the index cache),
	 * which is critical for concurrent index builds: a yield
	 * here would change fiber scheduling and could cause the
	 * build trigger to be removed while DML fibers are still
	 * in flight.
	 */
	struct vy_index_block_dir *dir = &run->block_dir[block_no];
	if (!dir->has_filter)
		return 0; /* No filter available, must check data. */

	*checked = true;
	uint64_t h = vy_stmt_hash64(key, key_def);
	if (binary_fuse8_contain(h, &dir->filter))
		return 0; /* Key might be present. */

	return 1; /* Key is definitely absent. */
}

/**
 * Read a page from disk given its number.
 * The function caches two most recently read pages.
 *
 * @retval 0 success
 * @retval -1 critical error
 */
static NODISCARD int
vy_run_iterator_load_page(struct vy_run_iterator *itr, uint32_t page_no,
			  struct vy_entry key, enum iterator_type iterator_type,
			  struct vy_page **result, uint32_t *pos_in_page,
			  bool *equal_found)
{
	struct vy_slice *slice = itr->slice;
	struct vy_run_env *env = slice->run->env;

	/* Check cache */
	struct vy_page *page = NULL;
	if (itr->curr_page != NULL &&
	    itr->curr_page->page_no == page_no) {
		page = itr->curr_page;
	} else if (itr->prev_page != NULL &&
		   itr->prev_page->page_no == page_no) {
		SWAP(itr->prev_page, itr->curr_page);
		page = itr->curr_page;
	}
	if (page != NULL) {
		if (key.stmt != NULL &&
		    vy_page_find_key(page, key, itr->cmp_def,
				     itr->format, iterator_type,
				     pos_in_page, equal_found) != 0)
			return -1;
		*result = page;
		return 0;
	}

	/* Allocate buffers */
	struct vy_page_info *page_info;
	page_info = vy_run_page_info_v2(slice->run, page_no, itr->cmp_def);
	if (page_info == NULL)
		return -1;
	page = vy_page_new(page_info);
	if (page == NULL)
		return -1;

	/* Read page data from the disk */
	struct vy_page_read_task *task = mempool_alloc(&env->read_task_pool);
	if (task == NULL) {
		diag_set(OutOfMemory, sizeof(*task),
			 "mempool", "vy_page_read_task");
		vy_page_delete(page);
		return -1;
	}
	task->run = slice->run;
	task->page_info = *page_info;
	task->page = page;
	task->key = key;
	task->iterator_type = iterator_type;
	task->cmp_def = itr->cmp_def;
	task->format = itr->format;
	task->pos_in_page = 0;
	task->equal_found = false;

	int rc = vy_run_env_coio_call(env, &task->base, vy_page_read_cb);

	*pos_in_page = task->pos_in_page;
	*equal_found = task->equal_found;

	/* Update read statistics from the task's copy of page_info,
	 * since the original pointer may have been invalidated by
	 * index cache eviction during the yield. */
	uint32_t row_count = task->page_info.row_count;
	uint32_t unpacked_size = task->page_info.unpacked_size;
	uint32_t compressed_size = task->page_info.size;

	mempool_free(&env->read_task_pool, task);
	if (rc != 0) {
		vy_page_delete(page);
		return -1;
	}

	/* Update cache */
	if (itr->prev_page != NULL)
		vy_page_delete(itr->prev_page);
	itr->prev_page = itr->curr_page;
	itr->curr_page = page;
	page->page_no = page_no;

	/* Update read statistics. */
	itr->stat->read.rows += row_count;
	itr->stat->read.bytes += unpacked_size;
	itr->stat->read.bytes_compressed += compressed_size;
	itr->stat->read.pages++;

	*result = page;
	return 0;
}

/**
 * Read key and lsn by a given wide position.
 * For the first record in a page reads the result from the page
 * index instead of fetching it from disk.
 *
 * @retval 0 success
 * @retval -1 read error or out of memory.
 */
static NODISCARD int
vy_run_iterator_read(struct vy_run_iterator *itr,
		     struct vy_run_iterator_pos pos,
		     struct vy_entry *ret)
{
	struct vy_page *page;
	bool equal_found;
	uint32_t pos_in_page;
	int rc = vy_run_iterator_load_page(itr, pos.page_no, vy_entry_none(),
					   ITER_GE, &page, &pos_in_page,
					   &equal_found);
	if (rc != 0)
		return rc;
	*ret = vy_page_stmt(page, pos.pos_in_page, itr->cmp_def, itr->format);
	if (ret->stmt == NULL)
		return -1;
	return 0;
}

/**
 * Binary search in a run for the given key.
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Resulting wide position is stored it *pos argument
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 *
 * @retval 0 success
 * @retval 1 EOF
 * @retval -1 read or memory error
 */
static NODISCARD int
vy_run_iterator_search(struct vy_run_iterator *itr,
		       enum iterator_type iterator_type, struct vy_entry key,
		       struct vy_run_iterator_pos *pos, bool *equal_key)
{
	if (vy_page_index_find_page(itr->slice->run, key,
				    itr->cmp_def, iterator_type,
				    equal_key, &pos->page_no) != 0)
		return -1;
	if (pos->page_no == itr->slice->run->info.page_count)
		return 1;
	bool equal_in_page;
	struct vy_page *page;
	int rc = vy_run_iterator_load_page(itr, pos->page_no, key,
					   iterator_type, &page,
					   &pos->pos_in_page, &equal_in_page);
	if (rc != 0)
		return rc;
	if (pos->pos_in_page == page->row_count) {
		pos->page_no++;
		pos->pos_in_page = 0;
	} else {
		*equal_key = equal_in_page;
	}
	return 0;
}

/**
 * Increment (or decrement, depending on the order) the current
 * wide position.
 * @retval 0 success, set *pos to new value
 * @retval 1 EOF
 * Affects: curr_loaded_page
 */
static NODISCARD int
vy_run_iterator_next_pos(struct vy_run_iterator *itr,
			 enum iterator_type iterator_type,
			 struct vy_run_iterator_pos *pos)
{
	struct vy_run *run = itr->slice->run;
	*pos = itr->curr_pos;
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		assert(pos->page_no <= run->info.page_count);
		if (pos->pos_in_page > 0) {
			pos->pos_in_page--;
		} else {
			if (pos->page_no == 0)
				return 1;
			pos->page_no--;
			struct vy_page_info *page_info;
			page_info = vy_run_page_info_v2(run, pos->page_no,
							itr->cmp_def);
			if (page_info == NULL)
				return -1;
			assert(page_info->row_count > 0);
			pos->pos_in_page = page_info->row_count - 1;
		}
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		assert(pos->page_no < run->info.page_count);
		struct vy_page_info *page_info;
		page_info = vy_run_page_info_v2(run, pos->page_no,
						itr->cmp_def);
		if (page_info == NULL)
			return -1;
		assert(page_info->row_count > 0);
		pos->pos_in_page++;
		if (pos->pos_in_page >= page_info->row_count) {
			pos->page_no++;
			pos->pos_in_page = 0;
			if (pos->page_no == run->info.page_count)
				return 1;
		}
	}
	return 0;
}

/**
 * Find the next record with lsn <= itr->lsn record.
 * The current position must be at the beginning of a series of
 * records with the same key it terms of direction of iterator
 * (i.e. left for GE, right for LE).
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read or memory error
 * Affects: curr_loaded_page, curr_pos
 */
static NODISCARD int
vy_run_iterator_find_lsn(struct vy_run_iterator *itr, struct vy_entry *ret)
{
	struct vy_slice *slice = itr->slice;
	struct key_def *cmp_def = itr->cmp_def;

	*ret = vy_entry_none();

	assert(itr->search_started);
	assert(itr->curr.stmt != NULL);
	assert(itr->curr_pos.page_no < slice->run->info.page_count);

	while (vy_stmt_lsn(itr->curr.stmt) > (**itr->read_view).vlsn ||
	       vy_stmt_flags(itr->curr.stmt) & VY_STMT_SKIP_READ) {
		if (vy_run_iterator_next_pos(itr, itr->iterator_type,
					     &itr->curr_pos) != 0) {
			vy_run_iterator_stop(itr);
			return 0;
		}
		tuple_unref(itr->curr.stmt);
		itr->curr = vy_entry_none();
		if (vy_run_iterator_read(itr, itr->curr_pos, &itr->curr) != 0)
			return -1;
		if (itr->iterator_type == ITER_EQ &&
		    vy_entry_compare(itr->curr, itr->key, cmp_def) != 0) {
			vy_run_iterator_stop(itr);
			return 0;
		}
	}
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		struct vy_run_iterator_pos test_pos;
		while (vy_run_iterator_next_pos(itr, itr->iterator_type,
						&test_pos) == 0) {
			struct vy_entry test;
			if (vy_run_iterator_read(itr, test_pos, &test) != 0)
				return -1;
			if (vy_stmt_lsn(test.stmt) > (**itr->read_view).vlsn ||
			    vy_stmt_flags(test.stmt) & VY_STMT_SKIP_READ ||
			    vy_entry_compare(itr->curr, test, cmp_def) != 0) {
				tuple_unref(test.stmt);
				break;
			}
			tuple_unref(itr->curr.stmt);
			itr->curr = test;
			itr->curr_pos = test_pos;
		}
	}
	/* Check if the result is within the slice boundaries. */
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		if (vy_entry_compare(itr->curr, slice->begin, cmp_def) < 0) {
			vy_run_iterator_stop(itr);
			return 0;
		}
	} else {
		assert(itr->iterator_type == ITER_GE ||
		       itr->iterator_type == ITER_GT ||
		       itr->iterator_type == ITER_EQ);
		if (vy_bound_cmp(itr->curr, slice->end_bound, cmp_def) > 0) {
			vy_run_iterator_stop(itr);
			return 0;
		}
	}
	vy_stmt_counter_acct_tuple(&itr->stat->get, itr->curr.stmt);
	*ret = itr->curr;
	return 0;
}

/**
 * Helper function for vy_run_iterator_seek().
 *
 * Positions the iterator to the beginning (i.e. leftmost for GE,
 * rightmost for LE) of a series of statements matching the given
 * search criteria.
 *
 * Updates itr->curr_pos. Doesn't affect itr->curr.
 *
 * @retval 0 success
 * @retval 1 EOF
 * @retval -1 read or memory error
 */
static NODISCARD int
vy_run_iterator_do_seek(struct vy_run_iterator *itr,
			enum iterator_type iterator_type, struct vy_entry key)
{
	struct vy_run *run = itr->slice->run;
	struct vy_run_iterator_pos end_pos = {run->info.page_count, 0};
	bool equal_found = false;
	if (!vy_stmt_is_empty_key(key.stmt)) {
		int rc = vy_run_iterator_search(itr, iterator_type, key,
						&itr->curr_pos, &equal_found);
		if (rc != 0)
			return rc;
	} else if (iterator_type == ITER_LE) {
		itr->curr_pos = end_pos;
	} else {
		assert(iterator_type == ITER_GE);
		itr->curr_pos.page_no = 0;
		itr->curr_pos.pos_in_page = 0;
	}
	if (iterator_type == ITER_EQ && !equal_found)
		return 1;
	if ((iterator_type == ITER_GE || iterator_type == ITER_GT) &&
	    itr->curr_pos.page_no == end_pos.page_no)
		return 1;
	if (iterator_type == ITER_LT || iterator_type == ITER_LE) {
		/**
		 * 1) in case of ITER_LT we now positioned on the value >= than
		 * given, so we need to make a step on previous key
		 * 2) in case if ITER_LE we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need to make a step on previous key
		 */
		return vy_run_iterator_next_pos(itr, iterator_type,
						&itr->curr_pos);
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		/**
		 * 1) in case of ITER_GT we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need just to find proper lsn
		 * 2) in case if ITER_GE or ITER_EQ we now positioned on the
		 * value >= given, so we need just to find proper lsn
		 */
		return 0;
	}
}

/**
 * Position the iterator to the first statement satisfying
 * the iterator search criteria and following the given key
 * (pass NULL to start iteration).
 */
static NODISCARD int
vy_run_iterator_seek(struct vy_run_iterator *itr, struct vy_entry last,
		     struct vy_entry *ret)
{
	struct key_def *cmp_def = itr->cmp_def;
	struct vy_slice *slice = itr->slice;
	struct vy_entry key = itr->key;
	enum iterator_type iterator_type = itr->iterator_type;

	*ret = vy_entry_none();
	assert(itr->search_started);

	/*
	 * Check the per-block binary fuse filter on the first
	 * iteration for EQ queries.
	 */
	bool check_bloom = false;
	if (itr->iterator_type == ITER_EQ && itr->curr.stmt == NULL) {
		int filter_rc = vy_run_bloom_check(slice->run, itr->key,
						   itr->key_def, cmp_def,
						   &check_bloom);
		if (filter_rc < 0)
			return -1;
		if (filter_rc > 0) {
			vy_run_iterator_stop(itr);
			itr->stat->bloom_hit++;
			return 0;
		}
	}

	/*
	 * vy_run_iterator_do_seek() implements its own EQ check.
	 * We only need to check EQ here if iterator type and key
	 * passed to it differ from the original.
	 */
	bool check_eq = false;

	/*
	 * Modify iterator type and key so as to position it to
	 * the first statement following the given key.
	 */
	if (last.stmt != NULL) {
		if (iterator_type == ITER_EQ)
			check_eq = true;
		iterator_type = iterator_direction(iterator_type) > 0 ?
				ITER_GT : ITER_LT;
		key = last;
	}

	/* Take slice boundaries into account. */
	if (iterator_type == ITER_GT || iterator_type == ITER_GE ||
	    iterator_type == ITER_EQ) {
		/*
		 *    original   |     start
		 * --------------+-------+-----+
		 *   KEY   | DIR |  KEY  | DIR |
		 * --------+-----+-------+-----+
		 * > begin | *   | key   | *   |
		 * = begin | gt  | key   | gt  |
		 *         | ge  | begin | ge  |
		 *         | eq  | begin | ge  |
		 * < begin | gt  | begin | ge  |
		 *         | ge  | begin | ge  |
		 *         | eq  |    stop     |
		 */
		int cmp = vy_entry_compare(key, slice->begin, cmp_def);
		if (cmp < 0 && iterator_type == ITER_EQ) {
			vy_run_iterator_stop(itr);
			return 0;
		}
		if (cmp < 0 || (cmp == 0 && iterator_type != ITER_GT)) {
			if (iterator_type == ITER_EQ)
				check_eq = true;
			iterator_type = ITER_GE;
			key = slice->begin;
		}
	}
	if (iterator_type == ITER_LT || iterator_type == ITER_LE) {
		/*
		 * Clamp the backward seek to the slice's end bound.
		 * If key is past end_bound in the bound-comparison
		 * sense, start from end_bound instead.
		 */
		if (vy_bound_cmp(key, slice->end_bound, cmp_def) > 0) {
			iterator_type = vy_entry_is_exclusive(
				slice->end_bound) ? ITER_LT : ITER_LE;
			key = slice->end_bound;
		}
	}

	/* Perform a lookup in the run. */
	itr->stat->lookup++;
	int rc = vy_run_iterator_do_seek(itr, iterator_type, key);
	if (rc < 0)
		return -1;
	if (rc > 0)
		goto not_found;

	/* Load the found statement. */
	if (itr->curr.stmt != NULL) {
		tuple_unref(itr->curr.stmt);
		itr->curr = vy_entry_none();
	}
	if (vy_run_iterator_read(itr, itr->curr_pos, &itr->curr) != 0)
		return -1;

	/* Check EQ constraint if necessary. */
	if (check_eq && vy_entry_compare(itr->curr, itr->key,
					 itr->cmp_def) != 0)
		goto not_found;

	/* Skip statements invisible from the iterator read view. */
	return vy_run_iterator_find_lsn(itr, ret);

not_found:
	if (check_bloom)
		itr->stat->bloom_miss++;
	vy_run_iterator_stop(itr);
	return 0;
}

/* }}} vy_run_iterator vy_run_iterator support functions */

/* {{{ vy_run_iterator API implementation */

void
vy_run_iterator_open(struct vy_run_iterator *itr,
		     struct vy_run_iterator_stat *stat,
		     struct vy_slice *slice, enum iterator_type iterator_type,
		     struct vy_entry key, const struct vy_read_view **rv,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct tuple_format *format)
{
	itr->stat = stat;
	itr->cmp_def = cmp_def;
	itr->key_def = key_def;
	itr->format = format;
	itr->slice = slice;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->read_view = rv;

	itr->curr = vy_entry_none();
	itr->curr_pos.page_no = slice->run->info.page_count;
	itr->curr_page = NULL;
	itr->prev_page = NULL;
	itr->search_started = false;

	/*
	 * Make sure the format we use to create tuples won't
	 * go away if DDL is called while the iterator is used.
	 *
	 * XXX: Please remove this kludge when proper DDL locking
	 * is implemented on transaction management level or multi
	 * version data dictionary is in place.
	 */
	tuple_format_ref(format);
}

/**
 * Advance a run iterator to the newest statement for the next key.
 * The statement is returned in @ret (NULL if EOF).
 * Returns 0 on success, -1 on memory allocation or IO error.
 */
static NODISCARD int
vy_run_iterator_next_key(struct vy_run_iterator *itr, struct vy_entry *ret)
{
	*ret = vy_entry_none();

	if (!itr->search_started) {
		itr->search_started = true;
		return vy_run_iterator_seek(itr, vy_entry_none(), ret);
	}
	if (itr->curr.stmt == NULL)
		return 0;

	assert(itr->curr_pos.page_no < itr->slice->run->info.page_count);

	struct vy_entry next = vy_entry_none();
	do {
		if (next.stmt != NULL)
			tuple_unref(next.stmt);
		if (vy_run_iterator_next_pos(itr, itr->iterator_type,
					     &itr->curr_pos) != 0) {
			vy_run_iterator_stop(itr);
			return 0;
		}

		if (vy_run_iterator_read(itr, itr->curr_pos, &next) != 0)
			return -1;
	} while (vy_entry_compare(itr->curr, next, itr->cmp_def) == 0);

	tuple_unref(itr->curr.stmt);
	itr->curr = next;

	if (itr->iterator_type == ITER_EQ &&
	    vy_entry_compare(next, itr->key, itr->cmp_def) != 0) {
		vy_run_iterator_stop(itr);
		return 0;
	}
	return vy_run_iterator_find_lsn(itr, ret);
}

/**
 * Advance a run iterator to the next (older) statement for the
 * current key. The statement is returned in @ret (NULL if EOF).
 * Returns 0 on success, -1 on memory allocation or IO error.
 */
static NODISCARD int
vy_run_iterator_next_lsn(struct vy_run_iterator *itr, struct vy_entry *ret)
{
	*ret = vy_entry_none();

	assert(itr->search_started);
	assert(itr->curr.stmt != NULL);
	assert(itr->curr_pos.page_no < itr->slice->run->info.page_count);

	struct vy_run_iterator_pos next_pos;
next:
	if (vy_run_iterator_next_pos(itr, ITER_GE, &next_pos) != 0)
		return 0;

	struct vy_entry next;
	if (vy_run_iterator_read(itr, next_pos, &next) != 0)
		return -1;

	if (vy_entry_compare(itr->curr, next, itr->cmp_def) != 0) {
		tuple_unref(next.stmt);
		return 0;
	}

	tuple_unref(itr->curr.stmt);
	itr->curr = next;
	itr->curr_pos = next_pos;
	if (vy_stmt_flags(itr->curr.stmt) & VY_STMT_SKIP_READ)
		goto next;

	vy_stmt_counter_acct_tuple(&itr->stat->get, itr->curr.stmt);
	*ret = itr->curr;
	return 0;
}

NODISCARD int
vy_run_iterator_next(struct vy_run_iterator *itr,
		     struct vy_history *history)
{
	vy_history_cleanup(history);
	struct vy_entry entry;
	if (vy_run_iterator_next_key(itr, &entry) != 0)
		return -1;
	while (entry.stmt != NULL) {
		if (vy_history_append_stmt(history, entry) != 0)
			return -1;
		if (vy_history_is_terminal(history))
			break;
		if (vy_run_iterator_next_lsn(itr, &entry) != 0)
			return -1;
	}
	return 0;
}

NODISCARD int
vy_run_iterator_skip(struct vy_run_iterator *itr, struct vy_entry last,
		     struct vy_history *history)
{
	/*
	 * Check if the iterator is already positioned
	 * at the statement following last.
	 */
	if (itr->search_started &&
	    (itr->curr.stmt == NULL || last.stmt == NULL ||
	     iterator_direction(itr->iterator_type) *
	     vy_entry_compare(itr->curr, last, itr->cmp_def) > 0))
		return 0;

	vy_history_cleanup(history);

	itr->search_started = true;
	struct vy_entry entry;
	if (vy_run_iterator_seek(itr, last, &entry) != 0)
		return -1;

	while (entry.stmt != NULL) {
		if (vy_history_append_stmt(history, entry) != 0)
			return -1;
		if (vy_history_is_terminal(history))
			break;
		if (vy_run_iterator_next_lsn(itr, &entry) != 0)
			return -1;
	}
	return 0;
}

void
vy_run_iterator_close(struct vy_run_iterator *itr)
{
	vy_run_iterator_stop(itr);
	tuple_format_unref(itr->format);
	TRASH(itr);
}

/* }}} vy_run_iterator API implementation */

/** Account a page to run statistics. */
static void
vy_run_acct_page(struct vy_run *run, struct vy_page_info *page)
{
	const char *min_key_end = page->min_key;
	mp_next(&min_key_end);
	run->page_index_size += sizeof(struct vy_page_info);
	run->page_index_size += min_key_end - page->min_key;
	run->count.rows += page->row_count;
	run->count.bytes += page->unpacked_size;
	run->count.bytes_compressed += page->size;
	run->count.pages++;
}

/**
 * Decode the block directory from the VY_RUN_INFO_BLOCK_DIR key
 * in the run_info header.  The directory is a msgpack array of
 * [boundary_key, page_count] pairs.
 *
 * @param run      Run to populate with block_dir / block_count.
 * @param data     Pointer to the msgpack array (advanced on return).
 * @param cmp_def  Key definition for hint computation.
 * @param filename For error reporting.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
int
vy_block_dir_decode(struct vy_run *run, const char **data,
		    struct key_def *cmp_def, const char *filename)
{
	uint32_t block_count = mp_decode_array(data);
	if (block_count == 0) {
		run->block_dir = NULL;
		run->block_count = 0;
		return 0;
	}
	run->block_dir = calloc(block_count, sizeof(struct vy_index_block_dir));
	if (run->block_dir == NULL) {
		diag_set(OutOfMemory,
			 block_count * sizeof(struct vy_index_block_dir),
			 "calloc", "struct vy_index_block_dir");
		return -1;
	}
	run->block_count = block_count;
	uint32_t page_no = 0;
	for (uint32_t b = 0; b < block_count; b++) {
		uint32_t arr_size = mp_decode_array(data);
		if (arr_size < 2) {
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 filename,
				 "Invalid block directory entry");
			return -1;
		}
		const char *key_beg = *data;
		mp_next(data);
		run->block_dir[b].boundary_key = vy_key_dup(key_beg);
		if (run->block_dir[b].boundary_key == NULL)
			return -1;
		uint32_t part_count = mp_decode_array(&key_beg);
		run->block_dir[b].boundary_key_hint =
			key_hint(key_beg, part_count, cmp_def);
		run->block_dir[b].page_count = mp_decode_uint(data);
		run->block_dir[b].first_page_no = page_no;
		page_no += run->block_dir[b].page_count;
		/* Skip unknown fields for forward compatibility. */
		for (uint32_t j = 2; j < arr_size; j++)
			mp_next(data);
	}
	return 0;
}

/**
 * Load block directory and metadata from a .index2 file.
 *
 * Reads the header row (run_info + block directory), then scans
 * the remaining rows (index blocks) to record their file offsets
 * in the block directory.  The .index2 file descriptor is kept
 * open for demand-loading of index blocks.
 *
 * If @a decode_run_info is false, run_info is already populated
 * (e.g. from the writer) and only block_dir is decoded.
 *
 * @retval  0 Success — run is populated with block_dir.
 * @retval -1 Error.
 * @retval  1 .index2 file not found (caller should rebuild it).
 */
static int
vy_run_load_index2(struct vy_run *run, const char *dir,
		   uint32_t space_id, uint32_t iid, struct key_def *cmp_def,
		   bool decode_run_info)
{
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dir,
			    space_id, iid, run->id, VY_FILE_INDEX2);

	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, path) != 0) {
		/*
		 * .index2 doesn't exist — not an error, just
		 * means this is a v1 run.
		 */
		struct error *e = diag_last_error(diag_get());
		if (e->type == &type_SystemError && errno == ENOENT)
			return 1;
		goto fail;
	}

	struct xlog_meta *meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_INDEX2) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_INDEX2, meta->filetype);
		goto fail_close;
	}

	/* Read the header transaction (first tx). */
	int rc = xlog_cursor_next_tx(&cursor);
	if (rc != 0) {
		if (rc > 0)
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 path, "Unexpected end of file");
		goto fail_close;
	}
	struct xrow_header xrow;
	rc = xlog_cursor_next_row(&cursor, &xrow);
	if (rc != 0) {
		if (rc > 0)
			diag_set(ClientError, ER_INVALID_INDEX_FILE,
				 path, "Unexpected end of file");
		goto fail_close;
	}
	if (xrow.type != VY_INDEX_RUN_INFO) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, path,
			 tt_sprintf("Wrong xrow type (expected %d, got %u)",
				    VY_INDEX_RUN_INFO, (unsigned)xrow.type));
		goto fail_close;
	}

	if (decode_run_info) {
		/*
		 * Decode run_info.  The standard decoder handles
		 * all known keys and skips unknown ones, so
		 * BLOCK_DIR will be silently skipped.  We decode
		 * it separately below.
		 */
		if (vy_run_info_decode(&run->info, &xrow, path) != 0)
			goto fail_close;
	}

	/*
	 * Re-scan the header body to find and decode BLOCK_DIR,
	 * RUN_COUNT, and PAGE_INDEX_SIZE.
	 */
	const char *pos = xrow.body->iov_base;
	uint32_t map_size = mp_decode_map(&pos);
	for (uint32_t i = 0; i < map_size; i++) {
		uint32_t key = mp_decode_uint(&pos);
		if (key == VY_RUN_INFO_BLOCK_DIR) {
			if (vy_block_dir_decode(run, &pos, cmp_def,
						path) != 0)
				goto fail_close;
		} else if (key == VY_RUN_INFO_RUN_COUNT) {
			uint32_t cnt_map = mp_decode_map(&pos);
			for (uint32_t j = 0; j < cnt_map; j++) {
				uint32_t ckey = mp_decode_uint(&pos);
				switch (ckey) {
				case 0:
					run->count.rows = mp_decode_uint(&pos);
					break;
				case 1:
					run->count.bytes = mp_decode_uint(&pos);
					break;
				case 2:
					run->count.bytes_compressed =
						mp_decode_uint(&pos);
					break;
				case 3:
					run->count.pages = mp_decode_uint(&pos);
					break;
				default:
					mp_next(&pos);
					break;
				}
			}
		} else if (key == VY_RUN_INFO_PAGE_INDEX_SIZE) {
			run->page_index_size = mp_decode_uint(&pos);
		} else if (key == VY_RUN_INFO_SKETCH) {
			uint32_t sketch_size = mp_decode_binl(&pos);
			if (sketch_size == MINHASH_SIZE) {
				memcpy(run->info.sketch.values, pos,
				       MINHASH_SIZE);
				run->info.has_sketch = true;
			}
			pos += sketch_size;
		} else {
			mp_next(&pos);
		}
	}

	if (run->block_dir == NULL) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE, path,
			 "Missing block directory");
		goto fail_close;
	}

	/*
	 * Scan the remaining transactions (index blocks) to record
	 * their file offsets and sizes in the block directory.
	 * Also extract the fuse8 filter from each block and store
	 * it in the block_dir entry.  Having the filter in RAM
	 * allows bloom checks without yielding to load the block
	 * from disk.
	 *
	 * Page info is demand-loaded from .index2 via the index
	 * cache (TX thread) or via blocking pread (compaction workers).
	 */
	for (uint32_t b = 0; b < run->block_count; b++) {
		uint64_t block_start = xlog_cursor_pos(&cursor);
		run->block_dir[b].file_offset = block_start;
		run->block_dir[b].has_filter = false;
		rc = xlog_cursor_next_tx(&cursor);
		if (rc != 0) {
			if (rc > 0)
				diag_set(ClientError, ER_INVALID_INDEX_FILE,
					 path, "Unexpected end of file "
					 "(fewer index blocks than expected)");
			goto fail_close;
		}
		run->block_dir[b].data_size =
			(uint32_t)(xlog_cursor_pos(&cursor) - block_start);
		/*
		 * Read the block row and extract its fuse8 filter.
		 * Failure is non-fatal: the block will still work
		 * via demand-loaded page info, just without the
		 * in-memory bloom optimization.
		 */
		struct xrow_header brow;
		if (xlog_cursor_next_row(&cursor, &brow) != 0)
			continue;
		if (brow.type != VY_INDEX_BLOCK)
			continue;
		const char *bpos = brow.body->iov_base;
		if (mp_typeof(*bpos) != MP_MAP)
			continue;
		uint32_t map_size = mp_decode_map(&bpos);
		for (uint32_t k = 0; k < map_size; k++) {
			uint32_t bkey = mp_decode_uint(&bpos);
			if (bkey == VY_INDEX_BLOCK_FILTER) {
				uint32_t fsize = mp_decode_binl(&bpos);
				if (fsize > 0 &&
				    binary_fuse8_deserialize(
					&run->block_dir[b].filter,
					bpos)) {
					run->block_dir[b].has_filter = true;
				}
				bpos += fsize;
			} else {
				mp_next(&bpos);
			}
		}
	}

	/*
	 * Keep the .index2 fd open for future demand-loading of
	 * index blocks via pread.
	 */
	run->index_fd = cursor.fd;
	xlog_cursor_close(&cursor, true);
	return 0;

fail_close:
	xlog_cursor_close(&cursor, false);
fail:
	/*
	 * Only free resources allocated by this function
	 * (block_dir).  Do NOT call vy_run_clear() here:
	 * this function may be called after a successful
	 * write, in which case the run's page_info, min_key,
	 * and max_key must be preserved for vy_run_write_index2.
	 *
	 * Callers that use this function for recovery
	 * (vy_run_recover_v2) do their own vy_run_clear()
	 * on failure.
	 */
	if (run->block_dir != NULL) {
		for (uint32_t i = 0; i < run->block_count; i++) {
			free(run->block_dir[i].boundary_key);
			if (run->block_dir[i].has_filter)
				binary_fuse8_free(&run->block_dir[i].filter);
		}
		free(run->block_dir);
	}
	run->block_dir = NULL;
	run->block_count = 0;
	return -1;
}

/**
 * Recover a run from a .index2 file.
 *
 * Loads the block directory via vy_run_load_index2(), then opens
 * the .run data file.
 *
 * @retval  0 Success — run is populated with block_dir.
 * @retval -1 Error.
 * @retval  1 .index2 file not found (caller should rebuild it).
 */
static int
vy_run_recover_v2(struct vy_run *run, const char *dir,
		  uint32_t space_id, uint32_t iid, struct key_def *cmp_def)
{
	ERROR_INJECT_COUNTDOWN(ERRINJ_VY_RUN_RECOVER_COUNTDOWN, {
		diag_set(ClientError, ER_INJECTION, "vinyl run recover");
		return -1;
	});
	int rc = vy_run_load_index2(run, dir, space_id, iid, cmp_def,
				    /*decode_run_info=*/true);
	if (rc != 0)
		return rc;

	/* Open the .run data file. */
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dir,
			    space_id, iid, run->id, VY_FILE_RUN);
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, path))
		goto fail;
	struct xlog_meta *meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_RUN) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_RUN, meta->filetype);
		xlog_cursor_close(&cursor, false);
		goto fail;
	}
	run->fd = cursor.fd;
	xlog_cursor_close(&cursor, true);
	return 0;

fail:
	vy_run_clear(run);
	return -1;
}

/* Forward declaration — defined further in the file. */
static int
vy_run_write_index2(struct vy_run *run, const char *dirpath,
		    uint32_t space_id, uint32_t iid,
		    struct vy_block_pds *pds_array);

int
vy_run_recover(struct vy_run *run, const char *dir,
	       uint32_t space_id, uint32_t iid, struct key_def *cmp_def,
	       struct tuple_format *format)
{
	int v2rc = vy_run_recover_v2(run, dir, space_id, iid, cmp_def);
	if (v2rc <= 0)
		return v2rc; /* 0 = success, -1 = error */

	/*
	 * .index2 file not found.  Build it by scanning the .run
	 * data file.  This handles both fresh installations and
	 * upgrades from the old .index format.
	 */
	if (vy_run_rebuild_index(run, dir, space_id, iid,
				 cmp_def, NULL, format, NULL) != 0) {
		say_error("failed to rebuild index for run %lld",
			  (long long)run->id);
		return -1;
	}

	/*
	 * .index2 has been written.  Clear the in-memory state
	 * and re-recover via the v2 path so that block_dir is
	 * set up for demand-loading.
	 *
	 * Close the fds opened by vy_run_rebuild_index before
	 * re-recovery opens them again.
	 */
	int64_t saved_id = run->id;
	if (run->fd >= 0) {
		close(run->fd);
		run->fd = -1;
	}
	if (run->index_fd >= 0) {
		close(run->index_fd);
		run->index_fd = -1;
	}
	vy_run_clear(run);
	run->id = saved_id;
	v2rc = vy_run_recover_v2(run, dir, space_id, iid, cmp_def);
	if (v2rc != 0) {
		say_error("failed to recover rebuilt .index2 for "
			  "run %lld", (long long)saved_id);
		return -1;
	}
	return 0;
}

/* dump statement to the run page buffers (stmt header and data) */
static int
vy_run_dump_stmt(struct vy_entry entry, struct xlog *data_xlog,
		 struct vy_page_info *info, struct key_def *key_def,
		 bool is_primary)
{
	struct xrow_header xrow;
	int rc = (is_primary ?
		  vy_stmt_encode_primary(entry.stmt, key_def, 0, &xrow) :
		  vy_stmt_encode_secondary(entry.stmt, key_def,
					   vy_entry_multikey_idx(entry, key_def),
					   &xrow));
	if (rc != 0)
		return -1;

	ssize_t row_size;
	if ((row_size = xlog_write_row(data_xlog, &xrow)) < 0)
		return -1;

	info->unpacked_size += row_size;
	info->row_count++;
	return 0;
}

/**
 * Encode uint32_t array of row offsets (row index) as xrow
 *
 * @param row_index row index
 * @param row_count size of row index
 * @param[out] xrow xrow to fill.
 * @retval 0 for success
 * @retval -1 for error
 */
static int
vy_row_index_encode(const uint32_t *row_index, uint32_t row_count,
		    struct xrow_header *xrow)
{
	memset(xrow, 0, sizeof(*xrow));
	xrow->type = VY_RUN_ROW_INDEX;

	size_t size = mp_sizeof_map(1) +
		      mp_sizeof_uint(VY_ROW_INDEX_DATA) +
		      mp_sizeof_bin(sizeof(uint32_t) * row_count);
	char *pos = region_alloc(&fiber()->gc, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "region", "row index");
		return -1;
	}
	xrow->body->iov_base = pos;
	pos = mp_encode_map(pos, 1);
	pos = mp_encode_uint(pos, VY_ROW_INDEX_DATA);
	pos = mp_encode_binl(pos, sizeof(uint32_t) * row_count);
	for (uint32_t i = 0; i < row_count; ++i)
		pos = mp_store_u32(pos, row_index[i]);
	xrow->body->iov_len = (void *)pos - xrow->body->iov_base;
	assert(xrow->body->iov_len == size);
	xrow->bodycnt = 1;
	return 0;
}

/**
 * Helper to extend run page info array
 */
static inline int
vy_run_alloc_page_info(struct vy_run *run, uint32_t *page_info_capacity)
{
	uint32_t cap = *page_info_capacity > 0 ?
		       *page_info_capacity * 2 : 16;
	struct vy_page_info *page_info = realloc(run->page_info,
					cap * sizeof(*page_info));
	if (page_info == NULL) {
		diag_set(OutOfMemory, cap * sizeof(*page_info),
			 "realloc", "struct vy_page_info");
		return -1;
	}
	run->page_info = page_info;
	*page_info_capacity = cap;
	return 0;
}

/** {{{ vy_page_info */

/** vy_page_info }}} */

/** {{{ vy_run_info */

/** Return the size of encoded statement statistics. */
static size_t
vy_stmt_stat_sizeof(const struct vy_stmt_stat *stat)
{
	return mp_sizeof_map(4) +
		mp_sizeof_uint(IPROTO_INSERT) +
		mp_sizeof_uint(IPROTO_REPLACE) +
		mp_sizeof_uint(IPROTO_DELETE) +
		mp_sizeof_uint(IPROTO_UPSERT) +
		mp_sizeof_uint(stat->inserts) +
		mp_sizeof_uint(stat->replaces) +
		mp_sizeof_uint(stat->deletes) +
		mp_sizeof_uint(stat->upserts);
}

/** Encode statement statistics to @buf and return advanced @buf. */
static char *
vy_stmt_stat_encode(const struct vy_stmt_stat *stat, char *buf)
{
	buf = mp_encode_map(buf, 4);
	buf = mp_encode_uint(buf, IPROTO_INSERT);
	buf = mp_encode_uint(buf, stat->inserts);
	buf = mp_encode_uint(buf, IPROTO_REPLACE);
	buf = mp_encode_uint(buf, stat->replaces);
	buf = mp_encode_uint(buf, IPROTO_DELETE);
	buf = mp_encode_uint(buf, stat->deletes);
	buf = mp_encode_uint(buf, IPROTO_UPSERT);
	buf = mp_encode_uint(buf, stat->upserts);
	return buf;
}

/* vy_run_info }}} */

/* {{{ .index2 v2 format writer */

/**
 * Encode a run_info header for the .index2 format.
 *
 * The header is the same as vy_run_info_encode() but with an
 * additional VY_RUN_INFO_BLOCK_DIR key: a msgpack array where
 * each element is a 2-element array [boundary_key, page_count].
 */
static int
vy_run_info_encode_v2(const struct vy_run *run, struct xrow_header *xrow)
{
	const struct vy_run_info *run_info = &run->info;
	const char *tmp;
	tmp = run_info->min_key;
	mp_next(&tmp);
	size_t min_key_size = tmp - run_info->min_key;
	tmp = run_info->max_key;
	mp_next(&tmp);
	size_t max_key_size = tmp - run_info->max_key;

	/*
	 * Build the block directory: an array of [boundary_key, page_count]
	 * pairs.  Compute boundary keys and page counts from the in-memory
	 * page_info array.
	 */
	uint32_t block_count = (run_info->page_count +
				VY_INDEX_BLOCK_SIZE - 1) / VY_INDEX_BLOCK_SIZE;

	/* Compute sizes of boundary keys. */
	size_t *bkey_sizes = NULL;
	if (block_count > 0) {
		bkey_sizes = region_alloc(&fiber()->gc,
					  block_count * sizeof(size_t));
		if (bkey_sizes == NULL) {
			diag_set(OutOfMemory, block_count * sizeof(size_t),
				 "region", "bkey_sizes");
			return -1;
		}
	}

	size_t block_dir_data_size = mp_sizeof_array(block_count);
	for (uint32_t b = 0; b < block_count; b++) {
		uint32_t first_page = b * VY_INDEX_BLOCK_SIZE;
		const struct vy_page_info *pi = &run->page_info[first_page];
		tmp = pi->min_key;
		mp_next(&tmp);
		bkey_sizes[b] = tmp - pi->min_key;
		uint32_t pages_in_block;
		if (b < block_count - 1)
			pages_in_block = VY_INDEX_BLOCK_SIZE;
		else
			pages_in_block = run_info->page_count -
					 first_page;
		block_dir_data_size += mp_sizeof_array(2) +
				       bkey_sizes[b] +
				       mp_sizeof_uint(pages_in_block);
	}

	uint32_t key_count = 9; /* 6 base + block_dir + run_count + page_index_size */
	if (run_info->has_sketch)
		key_count++;

	/*
	 * Compute RUN_COUNT map size: {rows, bytes, bytes_compressed, pages}
	 */
	size_t run_count_size = mp_sizeof_map(4) +
		mp_sizeof_uint(0) + mp_sizeof_uint(run->count.rows) +
		mp_sizeof_uint(1) + mp_sizeof_uint(run->count.bytes) +
		mp_sizeof_uint(2) + mp_sizeof_uint(run->count.bytes_compressed) +
		mp_sizeof_uint(3) + mp_sizeof_uint(run->count.pages);

	size_t size = mp_sizeof_map(key_count);
	size += mp_sizeof_uint(VY_RUN_INFO_MIN_KEY) + min_key_size;
	size += mp_sizeof_uint(VY_RUN_INFO_MAX_KEY) + max_key_size;
	size += mp_sizeof_uint(VY_RUN_INFO_MIN_LSN) +
		mp_sizeof_uint(run_info->min_lsn);
	size += mp_sizeof_uint(VY_RUN_INFO_MAX_LSN) +
		mp_sizeof_uint(run_info->max_lsn);
	size += mp_sizeof_uint(VY_RUN_INFO_PAGE_COUNT) +
		mp_sizeof_uint(run_info->page_count);
	size += mp_sizeof_uint(VY_RUN_INFO_STMT_STAT) +
		vy_stmt_stat_sizeof(&run_info->stmt_stat);
	size += mp_sizeof_uint(VY_RUN_INFO_BLOCK_DIR) + block_dir_data_size;
	size += mp_sizeof_uint(VY_RUN_INFO_RUN_COUNT) + run_count_size;
	size += mp_sizeof_uint(VY_RUN_INFO_PAGE_INDEX_SIZE) +
		mp_sizeof_uint(run->page_index_size);
	if (run_info->has_sketch)
		size += mp_sizeof_uint(VY_RUN_INFO_SKETCH) +
			mp_sizeof_bin(MINHASH_SIZE);

	char *pos = region_alloc(&fiber()->gc, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "region", "run info v2 encode");
		return -1;
	}
	memset(xrow, 0, sizeof(*xrow));
	xrow->body->iov_base = pos;

	pos = mp_encode_map(pos, key_count);

	pos = mp_encode_uint(pos, VY_RUN_INFO_MIN_KEY);
	memcpy(pos, run_info->min_key, min_key_size);
	pos += min_key_size;

	pos = mp_encode_uint(pos, VY_RUN_INFO_MAX_KEY);
	memcpy(pos, run_info->max_key, max_key_size);
	pos += max_key_size;

	pos = mp_encode_uint(pos, VY_RUN_INFO_MIN_LSN);
	pos = mp_encode_uint(pos, run_info->min_lsn);

	pos = mp_encode_uint(pos, VY_RUN_INFO_MAX_LSN);
	pos = mp_encode_uint(pos, run_info->max_lsn);

	pos = mp_encode_uint(pos, VY_RUN_INFO_PAGE_COUNT);
	pos = mp_encode_uint(pos, run_info->page_count);


	pos = mp_encode_uint(pos, VY_RUN_INFO_STMT_STAT);
	pos = vy_stmt_stat_encode(&run_info->stmt_stat, pos);

	/* Encode block directory. */
	pos = mp_encode_uint(pos, VY_RUN_INFO_BLOCK_DIR);
	pos = mp_encode_array(pos, block_count);
	for (uint32_t b = 0; b < block_count; b++) {
		uint32_t first_page = b * VY_INDEX_BLOCK_SIZE;
		const struct vy_page_info *pi = &run->page_info[first_page];
		uint32_t pages_in_block;
		if (b < block_count - 1)
			pages_in_block = VY_INDEX_BLOCK_SIZE;
		else
			pages_in_block = run_info->page_count - first_page;
		pos = mp_encode_array(pos, 2);
		memcpy(pos, pi->min_key, bkey_sizes[b]);
		pos += bkey_sizes[b];
		pos = mp_encode_uint(pos, pages_in_block);
	}

	/* Encode aggregate run count. */
	pos = mp_encode_uint(pos, VY_RUN_INFO_RUN_COUNT);
	pos = mp_encode_map(pos, 4);
	pos = mp_encode_uint(pos, 0);
	pos = mp_encode_uint(pos, run->count.rows);
	pos = mp_encode_uint(pos, 1);
	pos = mp_encode_uint(pos, run->count.bytes);
	pos = mp_encode_uint(pos, 2);
	pos = mp_encode_uint(pos, run->count.bytes_compressed);
	pos = mp_encode_uint(pos, 3);
	pos = mp_encode_uint(pos, run->count.pages);

	/* Encode page_index_size. */
	pos = mp_encode_uint(pos, VY_RUN_INFO_PAGE_INDEX_SIZE);
	pos = mp_encode_uint(pos, run->page_index_size);

	/* Encode run-level MinHash sketch. */
	if (run_info->has_sketch) {
		pos = mp_encode_uint(pos, VY_RUN_INFO_SKETCH);
		pos = mp_encode_binl(pos, MINHASH_SIZE);
		memcpy(pos, run_info->sketch.values, MINHASH_SIZE);
		pos += MINHASH_SIZE;
	}

	xrow->body->iov_len = (void *)pos - xrow->body->iov_base;
	xrow->bodycnt = 1;
	xrow->type = VY_INDEX_RUN_INFO;
	return 0;
}

/**
 * Encode a single index block as a msgpack map.
 *
 * The map has the following keys:
 *   VY_INDEX_BLOCK_PAGE_INFO  → array of page_info maps
 *   VY_INDEX_BLOCK_FILTER → binary fuse8 filter blob (if available)
 *   VY_INDEX_BLOCK_SKETCH → MinHash sketch blob (if available)
 *
 * @param run       Run whose page_info array to read from.
 * @param first     Global page number of the first page in the block.
 * @param count     Number of pages in this block.
 * @param pds       Per-block PDS, or NULL if not available.
 * @param[out] xrow Xrow header to fill in.
 *
 * @retval  0 Success.
 * @retval -1 Error.
 */
static int
vy_index_block_encode(struct vy_run *run, uint32_t first, uint32_t count,
		      struct vy_block_pds *pds, struct xrow_header *xrow)
{
	struct region *region = &fiber()->gc;

	/*
	 * First pass: compute the total size of the pages array.
	 */
	size_t pages_size = mp_sizeof_array(count);
	for (uint32_t i = 0; i < count; i++) {
		struct vy_page_info *pi = vy_run_page_info(run, first + i);
		const char *tmp = pi->min_key;
		mp_next(&tmp);
		size_t min_key_size = tmp - pi->min_key;
		pages_size += mp_sizeof_map(6) +
			mp_sizeof_uint(VY_PAGE_INFO_OFFSET) +
			mp_sizeof_uint(pi->offset) +
			mp_sizeof_uint(VY_PAGE_INFO_SIZE) +
			mp_sizeof_uint(pi->size) +
			mp_sizeof_uint(VY_PAGE_INFO_ROW_COUNT) +
			mp_sizeof_uint(pi->row_count) +
			mp_sizeof_uint(VY_PAGE_INFO_MIN_KEY) +
			min_key_size +
			mp_sizeof_uint(VY_PAGE_INFO_UNPACKED_SIZE) +
			mp_sizeof_uint(pi->unpacked_size) +
			mp_sizeof_uint(VY_PAGE_INFO_ROW_INDEX_OFFSET) +
			mp_sizeof_uint(pi->row_index_offset);
	}

	/* Compute filter and sketch blob sizes. */
	size_t filter_blob_size = 0;
	size_t sketch_blob_size = 0;
	uint32_t map_keys = 1; /* at least PAGES */
	if (pds != NULL && pds->has_filter) {
		filter_blob_size =
			binary_fuse8_serialization_bytes(&pds->filter);
		map_keys++;
	}
	if (pds != NULL && !minhash_is_empty(&pds->sketch)) {
		sketch_blob_size = MINHASH_SIZE;
		map_keys++;
	}

	/* Total size: outer map + pages key/val + filter key/val + sketch key/val */
	size_t total_size = mp_sizeof_map(map_keys);
	total_size += mp_sizeof_uint(VY_INDEX_BLOCK_PAGE_INFO) + pages_size;
	if (filter_blob_size > 0)
		total_size += mp_sizeof_uint(VY_INDEX_BLOCK_FILTER) +
			      mp_sizeof_bin(filter_blob_size);
	if (sketch_blob_size > 0)
		total_size += mp_sizeof_uint(VY_INDEX_BLOCK_SKETCH) +
			      mp_sizeof_bin(sketch_blob_size);

	char *buf = region_alloc(region, total_size);
	if (buf == NULL) {
		diag_set(OutOfMemory, total_size, "region",
			 "index block encode");
		return -1;
	}

	char *pos = buf;
	pos = mp_encode_map(pos, map_keys);

	/* Encode pages array. */
	pos = mp_encode_uint(pos, VY_INDEX_BLOCK_PAGE_INFO);
	pos = mp_encode_array(pos, count);
	for (uint32_t i = 0; i < count; i++) {
		struct vy_page_info *pi = vy_run_page_info(run, first + i);
		const char *tmp = pi->min_key;
		mp_next(&tmp);
		size_t min_key_size = tmp - pi->min_key;

		pos = mp_encode_map(pos, 6);
		pos = mp_encode_uint(pos, VY_PAGE_INFO_OFFSET);
		pos = mp_encode_uint(pos, pi->offset);
		pos = mp_encode_uint(pos, VY_PAGE_INFO_SIZE);
		pos = mp_encode_uint(pos, pi->size);
		pos = mp_encode_uint(pos, VY_PAGE_INFO_ROW_COUNT);
		pos = mp_encode_uint(pos, pi->row_count);
		pos = mp_encode_uint(pos, VY_PAGE_INFO_MIN_KEY);
		memcpy(pos, pi->min_key, min_key_size);
		pos += min_key_size;
		pos = mp_encode_uint(pos, VY_PAGE_INFO_UNPACKED_SIZE);
		pos = mp_encode_uint(pos, pi->unpacked_size);
		pos = mp_encode_uint(pos, VY_PAGE_INFO_ROW_INDEX_OFFSET);
		pos = mp_encode_uint(pos, pi->row_index_offset);
	}

	/* Encode filter blob. */
	if (filter_blob_size > 0) {
		pos = mp_encode_uint(pos, VY_INDEX_BLOCK_FILTER);
		pos = mp_encode_binl(pos, filter_blob_size);
		binary_fuse8_serialize(&pds->filter, pos);
		pos += filter_blob_size;
	}

	/* Encode sketch blob. */
	if (sketch_blob_size > 0) {
		pos = mp_encode_uint(pos, VY_INDEX_BLOCK_SKETCH);
		pos = mp_encode_binl(pos, sketch_blob_size);
		memcpy(pos, pds->sketch.values, sketch_blob_size);
		pos += sketch_blob_size;
	}

	assert((size_t)(pos - buf) == total_size);

	memset(xrow, 0, sizeof(*xrow));
	xrow->body->iov_base = buf;
	xrow->body->iov_len = total_size;
	xrow->bodycnt = 1;
	xrow->type = VY_INDEX_BLOCK;
	return 0;
}

/**
 * Write a .index2 file for the given run.
 *
 * The file contains:
 *   - Row 0 (header): run_info + block directory
 *   - Rows 1..K: index blocks (each is a batch of page_info entries)
 *
 * The header row is written as a standalone xlog transaction, followed
 * by one transaction per index block.  Each transaction has its own
 * CRC, enabling independent validation on demand-load.
 *
 * @param pds_array  Optional array of per-block PDS data.  If non-NULL,
 *                   must have at least block_count elements.  If NULL,
 *                   blocks are written without filter/sketch data.
 */
static int
vy_run_write_index2(struct vy_run *run, const char *dirpath,
		    uint32_t space_id, uint32_t iid,
		    struct vy_block_pds *pds_array)
{
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dirpath,
			    space_id, iid, run->id, VY_FILE_INDEX2);

	say_info("writing `%s'", path);

	struct xlog index_xlog;
	struct xlog_meta meta;
	xlog_meta_create(&meta, XLOG_META_TYPE_INDEX2, &INSTANCE_UUID,
			 NULL, NULL);
	struct xlog_opts opts = xlog_opts_default;
	opts.rate_limit = run->env->snap_io_rate_limit;
	opts.sync_interval = VY_RUN_SYNC_INTERVAL;
	if (xlog_create(&index_xlog, path, 0, &meta, &opts) < 0)
		return -1;

	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	struct xrow_header xrow;

	/* Write the header row (run_info + block directory). */
	xlog_tx_begin(&index_xlog);
	if (vy_run_info_encode_v2(run, &xrow) != 0 ||
	    xlog_write_row(&index_xlog, &xrow) < 0)
		goto fail_rollback;
	region_truncate(region, region_svp);
	if (xlog_tx_commit(&index_xlog) < 0 ||
	    xlog_flush(&index_xlog) < 0)
		goto fail;

	/* Write index blocks, one transaction per block. */
	uint32_t block_count = (run->info.page_count +
				VY_INDEX_BLOCK_SIZE - 1) / VY_INDEX_BLOCK_SIZE;
	for (uint32_t b = 0; b < block_count; b++) {
		uint32_t first_page = b * VY_INDEX_BLOCK_SIZE;
		uint32_t pages_in_block;
		if (b < block_count - 1)
			pages_in_block = VY_INDEX_BLOCK_SIZE;
		else
			pages_in_block = run->info.page_count - first_page;

		struct vy_block_pds *pds = pds_array != NULL ?
					   &pds_array[b] : NULL;
		xlog_tx_begin(&index_xlog);
		if (vy_index_block_encode(run, first_page, pages_in_block,
					  pds, &xrow) != 0 ||
		    xlog_write_row(&index_xlog, &xrow) < 0)
			goto fail_rollback;
		region_truncate(region, region_svp);
		if (xlog_tx_commit(&index_xlog) < 0 ||
		    xlog_flush(&index_xlog) < 0)
			goto fail;
	}

	if (xlog_flush(&index_xlog) < 0)
		goto fail;

	ERROR_INJECT(ERRINJ_VY_INDEX_FILE_RENAME, {
		diag_set(ClientError, ER_INJECTION,
			 "vinyl index file rename");
		goto fail;
	});

	if (xlog_rename(&index_xlog) < 0)
		goto fail;

	xlog_close(&index_xlog, false);
	return 0;

fail_rollback:
	region_truncate(region, region_svp);
	xlog_tx_rollback(&index_xlog);
fail:
	xlog_close(&index_xlog, false);
	unlink(path);
	return -1;
}

/* .index2 v2 format writer }}} */

int
vy_run_writer_create(struct vy_run_writer *writer, struct vy_run *run,
		     const char *dirpath, uint32_t space_id, uint32_t iid,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct index_opts *index_opts)
{
	memset(writer, 0, sizeof(*writer));
	writer->run = run;
	writer->dirpath = dirpath;
	writer->space_id = space_id;
	writer->iid = iid;
	writer->cmp_def = cmp_def;
	writer->key_def = key_def;
	writer->index_opts = *index_opts;
	xlog_clear(&writer->data_xlog);
	ibuf_create(&writer->row_index_buf, &cord()->slabc,
		    4096 * sizeof(uint32_t));
	run->info.min_lsn = INT64_MAX;
	run->info.max_lsn = -1;
	assert(run->page_info == NULL);
	/* Initialize per-block PDS accumulator. */
	minhash_create(&writer->block_sketch);
	return 0;
}

/**
 * Create an xlog to write run.
 * @param writer Run writer.
 * @retval -1 Memory or IO error.
 * @retval  0 Success.
 */
static int
vy_run_writer_create_xlog(struct vy_run_writer *writer)
{
	assert(!xlog_is_open(&writer->data_xlog));
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), writer->dirpath,
			    writer->space_id, writer->iid, writer->run->id,
			    VY_FILE_RUN);
	say_info("writing `%s'", path);
	struct xlog_meta meta;
	xlog_meta_create(&meta, XLOG_META_TYPE_RUN, &INSTANCE_UUID,
			 NULL, NULL);
	struct xlog_opts opts = xlog_opts_default;
	opts.rate_limit = writer->run->env->snap_io_rate_limit;
	opts.sync_interval = VY_RUN_SYNC_INTERVAL;
	opts.compression_level = writer->index_opts.compression_level;
	if (xlog_create(&writer->data_xlog, path, 0, &meta, &opts) != 0)
		return -1;
	return 0;
}

/* Forward declaration. */
static int
vy_run_writer_finalize_block_pds(struct vy_run_writer *writer);

/**
 * Start a new page with a min_key stored in @a first_entry.
 * @param writer Run writer.
 * @param first_entry First statement of a page.
 *
 * @retval -1 Memory error.
 * @retval  0 Success.
 */
static int
vy_run_writer_start_page(struct vy_run_writer *writer,
			 struct vy_entry first_entry)
{
	struct vy_run *run = writer->run;
	if (run->info.page_count >= writer->page_info_capacity &&
	    vy_run_alloc_page_info(run, &writer->page_info_capacity) != 0)
		return -1;
	const char *key = vy_stmt_is_key(first_entry.stmt) ?
			  tuple_data(first_entry.stmt) :
			  tuple_extract_key(first_entry.stmt, writer->cmp_def,
					    vy_entry_multikey_idx(first_entry,
								  writer->cmp_def),
					    NULL);
	if (key == NULL)
		return -1;
	if (run->info.page_count == 0) {
		assert(run->info.min_key == NULL);
		run->info.min_key = mp_dup(key);
	}
	struct vy_page_info *page = run->page_info + run->info.page_count;
	vy_page_info_create(page, writer->data_xlog.offset, key,
			    writer->cmp_def);
	run->info.page_count++;

	/*
	 * Check if we've just crossed an index block boundary.
	 * When page_count reaches a multiple of VY_INDEX_BLOCK_SIZE
	 * (and it's not the very first page), the previous block
	 * is complete and we can finalize its PDS.
	 */
	if (run->info.page_count > 1 &&
	    (run->info.page_count - 1) % VY_INDEX_BLOCK_SIZE == 0) {
		if (vy_run_writer_finalize_block_pds(writer) != 0)
			return -1;
	}

	xlog_tx_begin(&writer->data_xlog);
	return 0;
}

/**
 * Write @a stmt into a current page.
 * @param writer Run writer.
 * @param entry Statement to write.
 *
 * @retval -1 Memory or IO error.
 * @retval  0 Success.
 */
/**
 * Accumulate a 64-bit key hash for the current block's
 * binary fuse filter and MinHash sketch.
 */
static int
vy_run_writer_hash_stmt(struct vy_run_writer *writer, struct vy_entry entry)
{
	uint64_t h = vy_stmt_hash64(entry, writer->key_def);
	/* Grow the hash accumulator if needed. */
	if (writer->block_hash_count >= writer->block_hash_cap) {
		uint32_t new_cap = writer->block_hash_cap == 0 ?
				   4096 : writer->block_hash_cap * 2;
		uint64_t *new_buf = realloc(writer->block_hashes,
					    new_cap * sizeof(uint64_t));
		if (new_buf == NULL) {
			diag_set(OutOfMemory,
				 new_cap * sizeof(uint64_t),
				 "realloc", "block_hashes");
			return -1;
		}
		writer->block_hashes = new_buf;
		writer->block_hash_cap = new_cap;
	}
	writer->block_hashes[writer->block_hash_count++] = h;
	minhash_add(&writer->block_sketch, h);
	return 0;
}

/**
 * Finalize the PDS for the current index block: build a binary
 * fuse8 filter from the accumulated hashes, store the sketch,
 * and reset the accumulators for the next block.
 */
static int
vy_run_writer_finalize_block_pds(struct vy_run_writer *writer)
{
	/* Grow the block_pds array if needed. */
	if (writer->block_pds_count >= writer->block_pds_cap) {
		uint32_t new_cap = writer->block_pds_cap == 0 ?
				   16 : writer->block_pds_cap * 2;
		struct vy_block_pds *new_buf = realloc(
			writer->block_pds,
			new_cap * sizeof(struct vy_block_pds));
		if (new_buf == NULL) {
			diag_set(OutOfMemory,
				 new_cap * sizeof(struct vy_block_pds),
				 "realloc", "block_pds");
			return -1;
		}
		writer->block_pds = new_buf;
		writer->block_pds_cap = new_cap;
	}

	struct vy_block_pds *pds =
		&writer->block_pds[writer->block_pds_count];
	memset(pds, 0, sizeof(*pds));

	/*
	 * Build binary fuse8 filter.  binary_fuse8_populate
	 * requires at least 2 keys to build a filter.
	 */
	if (writer->block_hash_count > 1) {
		if (!binary_fuse8_allocate(writer->block_hash_count,
					    &pds->filter)) {
			/*
			 * Allocation failure is not fatal — we simply
			 * skip the filter for this block.
			 */
			pds->has_filter = false;
		} else {
			if (binary_fuse8_populate(writer->block_hashes,
						   writer->block_hash_count,
						  &pds->filter)) {
				pds->has_filter = true;
			} else {
				binary_fuse8_free(&pds->filter);
				pds->has_filter = false;
			}
		}
	}

	/* Copy the MinHash sketch. */
	pds->sketch = writer->block_sketch;

	writer->block_pds_count++;

	/* Reset accumulators for the next block. */
	writer->block_hash_count = 0;
	minhash_create(&writer->block_sketch);

	return 0;
}

static int
vy_run_writer_write_to_page(struct vy_run_writer *writer, struct vy_entry entry)
{
	if (vy_run_writer_hash_stmt(writer, entry) != 0)
		return -1;
	if (writer->last.stmt != NULL)
		vy_stmt_unref_if_possible(writer->last.stmt);
	writer->last = entry;
	vy_stmt_ref_if_possible(entry.stmt);
	struct vy_run *run = writer->run;
	struct vy_page_info *page = run->page_info + run->info.page_count - 1;
	uint32_t *offset = (uint32_t *)ibuf_alloc(&writer->row_index_buf,
						  sizeof(uint32_t));
	if (offset == NULL) {
		diag_set(OutOfMemory, sizeof(uint32_t), "ibuf", "row index");
		return -1;
	}
	*offset = page->unpacked_size;
	if (vy_run_dump_stmt(entry, &writer->data_xlog, page,
			     writer->cmp_def, writer->iid == 0) != 0)
		return -1;
	int64_t lsn = vy_stmt_lsn(entry.stmt);
	run->info.min_lsn = MIN(run->info.min_lsn, lsn);
	run->info.max_lsn = MAX(run->info.max_lsn, lsn);
	vy_stmt_stat_acct(&run->info.stmt_stat, vy_stmt_type(entry.stmt));
	return 0;
}

/**
 * Finish a current page.
 * @param writer Run writer.
 * @retval -1 Memory or IO error.
 * @retval  0 Success.
 */
static int
vy_run_writer_end_page(struct vy_run_writer *writer)
{
	struct vy_run *run = writer->run;
	assert(run->info.page_count > 0);
	struct vy_page_info *page = run->page_info + run->info.page_count - 1;

	assert(page->row_count > 0);
	assert(ibuf_used(&writer->row_index_buf) ==
	       sizeof(uint32_t) * page->row_count);

	struct xrow_header xrow;
	uint32_t *row_index = (uint32_t *)writer->row_index_buf.rpos;
	if (vy_row_index_encode(row_index, page->row_count, &xrow) < 0)
		return -1;
	ssize_t written = xlog_write_row(&writer->data_xlog, &xrow);
	if (written < 0)
		return -1;
	page->row_index_offset = page->unpacked_size;
	page->unpacked_size += written;

	written = xlog_tx_commit(&writer->data_xlog);
	if (written == 0)
		written = xlog_flush(&writer->data_xlog);
	if (written < 0)
		return -1;
	page->size = written;
	vy_run_acct_page(run, page);
	ibuf_reset(&writer->row_index_buf);
	return 0;
}

int
vy_run_writer_append_stmt(struct vy_run_writer *writer, struct vy_entry entry)
{
	int rc = -1;
	size_t region_svp = region_used(&fiber()->gc);
	if (!xlog_is_open(&writer->data_xlog) &&
	    vy_run_writer_create_xlog(writer) != 0)
		goto out;
	if (ibuf_used(&writer->row_index_buf) == 0 &&
	    vy_run_writer_start_page(writer, entry) != 0)
		goto out;
	if (vy_run_writer_write_to_page(writer, entry) != 0)
		goto out;
	size_t page_size = (size_t)writer->index_opts.page_size;
	if (obuf_size(&writer->data_xlog.obuf) >= page_size &&
	    vy_run_writer_end_page(writer) != 0)
		goto out;
	rc = 0;
out:
	region_truncate(&fiber()->gc, region_svp);
	return rc;
}

/**
 * Destroy a run writer.
 * @param writer Writer to destroy.
 * @param reuse_fd True in a case of success run write. And else
 *        false.
 */
static void
vy_run_writer_destroy(struct vy_run_writer *writer, bool reuse_fd)
{
	if (writer->last.stmt != NULL)
		vy_stmt_unref_if_possible(writer->last.stmt);
	if (xlog_is_open(&writer->data_xlog))
		xlog_close(&writer->data_xlog, reuse_fd);
	ibuf_destroy(&writer->row_index_buf);
	/* Free PDS accumulators. */
	free(writer->block_hashes);
	writer->block_hashes = NULL;
	if (writer->block_pds != NULL) {
		for (uint32_t i = 0; i < writer->block_pds_count; i++) {
			if (writer->block_pds[i].has_filter)
				binary_fuse8_free(&writer->block_pds[i].filter);
		}
		free(writer->block_pds);
		writer->block_pds = NULL;
	}
}

int
vy_run_writer_commit(struct vy_run_writer *writer)
{
	int rc = -1;
	size_t region_svp = region_used(&fiber()->gc);

	if (ibuf_used(&writer->row_index_buf) != 0 &&
	    vy_run_writer_end_page(writer) != 0)
		goto out;

	struct vy_run *run = writer->run;
	if (vy_run_is_empty(run)) {
		vy_run_writer_destroy(writer, false);
		rc = 0;
		goto out;
	}

	assert(writer->last.stmt != NULL);
	const char *key = vy_stmt_is_key(writer->last.stmt) ?
		          tuple_data(writer->last.stmt) :
			  tuple_extract_key(writer->last.stmt, writer->cmp_def,
					    vy_entry_multikey_idx(writer->last,
								  writer->cmp_def),
					    NULL);
	if (key == NULL)
		goto out;

	assert(run->info.max_key == NULL);
	run->info.max_key = mp_dup(key);

	ERROR_INJECT(ERRINJ_VY_RUN_FILE_RENAME, {
		diag_set(ClientError, ER_INJECTION, "vinyl run file rename");
		goto out;
	});

	/* Sync data and link the file to the final name. */
	if (xlog_sync(&writer->data_xlog) < 0 ||
	    xlog_rename(&writer->data_xlog) < 0)
		goto out;

	/*
	 * Finalize the last index block's PDS (the block that was
	 * being accumulated when we ran out of pages).
	 */
	if (writer->block_hash_count > 0) {
		if (vy_run_writer_finalize_block_pds(writer) != 0)
			goto out;
	}

	/*
	 * Merge all block sketches into a run-level sketch for
	 * use in compaction overlap estimation.
	 */
	minhash_create(&run->info.sketch);
	run->info.has_sketch = false;
	for (uint32_t i = 0; i < writer->block_pds_count; i++) {
		if (!minhash_is_empty(&writer->block_pds[i].sketch)) {
			minhash_merge(&run->info.sketch,
				      &writer->block_pds[i].sketch);
			run->info.has_sketch = true;
		}
	}

	/* Shrink to fit actual size. */
	uint32_t page_count = run->info.page_count;
	struct vy_page_info *new_pi = realloc(run->page_info,
					      page_count * sizeof(*new_pi));
	if (new_pi != NULL) {
		run->page_info = new_pi;
		writer->page_info_capacity = page_count;
	}

	if (vy_run_write_index2(run, writer->dirpath,
				writer->space_id, writer->iid,
				writer->block_pds) != 0)
		goto out;

	/*
	 * Load the freshly-written .index2 to populate block_dir.
	 * This must succeed — if it doesn't, the entire dump or
	 * compaction is considered failed, and the .run and .index2
	 * files will be garbage-collected.
	 *
	 * N.B.: must happen before vy_run_writer_destroy so that
	 * on failure the caller can call vy_run_writer_abort.
	 */
	if (vy_run_load_index2(run, writer->dirpath, writer->space_id,
			       writer->iid, writer->cmp_def,
			       /*decode_run_info=*/false) != 0)
		goto out;

	/*
	 * Free the in-memory page_info since pages are now
	 * demand-loaded from .index2 (or served from cache).
	 */
	free(run->page_info);
	run->page_info = NULL;

	run->fd = writer->data_xlog.fd;
	vy_run_writer_destroy(writer, true);
	rc = 0;
out:
	region_truncate(&fiber()->gc, region_svp);
	return rc;
}

void
vy_run_writer_abort(struct vy_run_writer *writer)
{
	vy_run_writer_destroy(writer, false);
}

int
vy_run_rebuild_index(struct vy_run *run, const char *dir,
		     uint32_t space_id, uint32_t iid,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct tuple_format *format, const struct index_opts *opts)
{
	(void)key_def;
	(void)opts;
	assert(run->page_info == NULL);
	struct region *region = &fiber()->gc;
	size_t mem_used = region_used(region);

	struct xlog_cursor cursor;
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dir,
			    space_id, iid, run->id, VY_FILE_RUN);

	say_info("rebuilding index for `%s'", path);
	if (xlog_cursor_open(&cursor, path))
		return -1;

	int rc = 0;
	uint32_t page_info_capacity = 0;

	const char *key = NULL;
	int64_t max_lsn = 0;
	int64_t min_lsn = INT64_MAX;
	struct tuple *prev_tuple = NULL;
	char *page_min_key = NULL;

	off_t page_offset, next_page_offset = xlog_cursor_pos(&cursor);
	while ((rc = xlog_cursor_next_tx(&cursor)) == 0) {
		region_truncate(region, mem_used);
		page_offset = next_page_offset;
		next_page_offset = xlog_cursor_pos(&cursor);

		if (run->info.page_count == page_info_capacity &&
		    vy_run_alloc_page_info(run, &page_info_capacity) != 0)
			goto close_err;
		uint32_t page_row_count = 0;
		uint64_t page_row_index_offset = 0;
		uint64_t row_offset = xlog_cursor_tx_pos(&cursor);

		struct xrow_header xrow;
		while ((rc = xlog_cursor_next_row(&cursor, &xrow)) == 0) {
			if (xrow.type == VY_RUN_ROW_INDEX) {
				page_row_index_offset = row_offset;
				row_offset = xlog_cursor_tx_pos(&cursor);
				continue;
			}
			++page_row_count;
			struct tuple *tuple = vy_stmt_decode(&xrow, format);
			if (tuple == NULL)
				goto close_err;
			key = vy_stmt_is_key(tuple) ? tuple_data(tuple) :
			      tuple_extract_key(tuple, cmp_def,
						MULTIKEY_NONE, NULL);
			if (prev_tuple != NULL)
				tuple_unref(prev_tuple);
			prev_tuple = tuple;
			if (key == NULL)
				goto close_err;
			if (run->info.min_key == NULL)
				run->info.min_key = mp_dup(key);
			if (page_min_key == NULL)
				page_min_key = mp_dup(key);
			if (xrow.lsn > max_lsn)
				max_lsn = xrow.lsn;
			if (xrow.lsn < min_lsn)
				min_lsn = xrow.lsn;
			row_offset = xlog_cursor_tx_pos(&cursor);
		}
		struct vy_page_info *info;
		info = run->page_info + run->info.page_count;
		vy_page_info_create(info, page_offset, page_min_key, cmp_def);
		info->row_count = page_row_count;
		info->size = next_page_offset - page_offset;
		info->unpacked_size = xlog_cursor_tx_pos(&cursor);
		info->row_index_offset = page_row_index_offset;
		++run->info.page_count;
		vy_run_acct_page(run, info);

		free(page_min_key);
		page_min_key = NULL;
	}

	if (key != NULL)
		run->info.max_key = mp_dup(key);
	run->info.max_lsn = max_lsn;
	run->info.min_lsn = min_lsn;

	if (prev_tuple != NULL) {
		tuple_unref(prev_tuple);
		prev_tuple = NULL;
	}
	region_truncate(region, mem_used);
	run->fd = cursor.fd;
	xlog_cursor_close(&cursor, true);

	/* New run index is ready for write, unlink old files if exist */
	vy_run_snprint_path(path, sizeof(path), dir,
			    space_id, iid, run->id, VY_FILE_INDEX);
	xlog_remove_file(path, 0);
	vy_run_snprint_path(path, sizeof(path), dir,
			    space_id, iid, run->id, VY_FILE_INDEX2);
	xlog_remove_file(path, 0);
	if (vy_run_write_index2(run, dir, space_id, iid, NULL) != 0)
		goto close_err;
	return 0;
close_err:
	vy_run_clear(run);
	region_truncate(region, mem_used);
	if (prev_tuple != NULL)
		tuple_unref(prev_tuple);
	if (page_min_key != NULL)
		free(page_min_key);
	if (xlog_cursor_is_open(&cursor))
		xlog_cursor_close(&cursor, false);
	return -1;
}

/**
 * Try to remove an empty directory.
 *
 * @param path Path to the directory.
 * @return int 0 on success, -1 on error.
 */
static int
try_rmdir(const char *path)
{
	int rc = 0;
	if (rmdir(path) < 0) {
		if (errno != ENOENT) {
			if (errno != ENOTEMPTY)
				say_syserror("error while removing %s", path);
			rc = -1;
		}
	} else {
		say_info("removed %s", path);
	}
	return rc;
}

static ssize_t
vy_run_remove_files_f(va_list ap)
{
	const char *dir = va_arg(ap, typeof(dir));
	uint32_t space_id = va_arg(ap, typeof(space_id));
	uint32_t iid = va_arg(ap, typeof(iid));
	int64_t run_id = va_arg(ap, typeof(run_id));

	ERROR_INJECT(ERRINJ_VY_GC,
		     {say_error("error injection: vinyl run %lld not deleted",
				(long long)run_id); return -1;});
	int ret = 0;
	char path[PATH_MAX];
	for (int type = 0; type < vy_file_MAX; type++) {
		vy_run_snprint_path(path, sizeof(path), dir,
				    space_id, iid, run_id, type);
		if (!xlog_remove_file(path, XLOG_RM_VERBOSE))
			ret = -1;
	}
	/* Remove the root directory if it's empty. */
	vy_lsm_snprint_path(path, sizeof(path), dir, space_id, iid);
	if (try_rmdir(path) < 0)
		return ret;
	vy_space_snprint_path(path, sizeof(path), dir, space_id);
	try_rmdir(path);
	return ret;
}

int
vy_run_remove_files(const char *dir, uint32_t space_id,
		    uint32_t iid, int64_t run_id)
{
	return coio_call(vy_run_remove_files_f, dir, space_id, iid, run_id);
}

/* ----------------------------------------------------------------
 * vy_compaction_stream — v2 compaction iterator for worker threads
 * ---------------------------------------------------------------- */


/**
 * Free the locally cached index block.
 */
static void
vy_compaction_stream_free_block(struct vy_compaction_stream *stream)
{
	if (stream->block_pages != NULL) {
		for (uint32_t i = 0; i < stream->block_page_count; i++) {
			if (stream->block_pages[i].min_key != NULL)
				free(stream->block_pages[i].min_key);
		}
		free(stream->block_pages);
		stream->block_pages = NULL;
		stream->block_page_count = 0;
	}
}

/**
 * Load the index block containing @a page_no from the .index2 file
 * via blocking pread (safe on a worker thread).
 */
static int
vy_compaction_stream_load_block(struct vy_compaction_stream *stream,
				uint32_t page_no)
{
	struct vy_run *run = stream->slice->run;
	assert(run->block_dir != NULL);

	uint32_t block_no = page_no / VY_INDEX_BLOCK_SIZE;
	if (block_no >= run->block_count)
		block_no = run->block_count - 1;
	struct vy_index_block_dir *dir = &run->block_dir[block_no];

	vy_compaction_stream_free_block(stream);

	size_t region_svp = region_used(&fiber()->gc);
	struct xlog_tx_cursor tx_cursor;
	struct xrow_header xrow;
	if (vy_index_block_read_xrow(run->index_fd, dir->file_offset,
				      dir->data_size, run->env,
				      &tx_cursor, &xrow) != 0)
		goto err;

	/*
	 * v2 index blocks are encoded as a map:
	 *   { VY_INDEX_BLOCK_PAGE_INFO: [page_info, ...], ... }
	 * Decode the outer map and find the pages array.
	 */
	const char *bpos = xrow.body->iov_base;
	uint32_t map_size = mp_decode_map(&bpos);
	const char *pages_data = NULL;
	for (uint32_t m = 0; m < map_size; m++) {
		uint32_t key = mp_decode_uint(&bpos);
		if (key == VY_INDEX_BLOCK_PAGE_INFO)
			pages_data = bpos;
		mp_next(&bpos);
	}
	if (pages_data == NULL) {
		diag_set(ClientError, ER_INVALID_INDEX_FILE,
			 "Missing PAGES key in index block");
		xlog_tx_cursor_destroy(&tx_cursor);
		goto err;
	}

	struct vy_page_info *pages;
	uint32_t arr_count;
	if (vy_index_block_decode_pages(pages_data, stream->cmp_def,
					 &pages, &arr_count, NULL) != 0) {
		xlog_tx_cursor_destroy(&tx_cursor);
		goto err;
	}

	xlog_tx_cursor_destroy(&tx_cursor);
	region_truncate(&fiber()->gc, region_svp);

	stream->block_pages = pages;
	stream->block_page_count = arr_count;
	stream->block_first_page_no = dir->first_page_no;
	return 0;
err:
	region_truncate(&fiber()->gc, region_svp);
	return -1;
}

/**
 * Get page_info for a given page number from the stream's
 * locally cached block, loading it if necessary.
 */
static struct vy_page_info *
vy_compaction_stream_page_info(struct vy_compaction_stream *stream,
			       uint32_t page_no)
{
	if (stream->block_pages == NULL ||
	    page_no < stream->block_first_page_no ||
	    page_no >= stream->block_first_page_no +
		       stream->block_page_count) {
		if (vy_compaction_stream_load_block(stream, page_no) != 0)
			return NULL;
	}
	uint32_t local = page_no - stream->block_first_page_no;
	assert(local < stream->block_page_count);
	return &stream->block_pages[local];
}

static NODISCARD int
vy_compaction_stream_read_page(struct vy_compaction_stream *stream)
{
	struct vy_run *run = stream->slice->run;
	assert(stream->page == NULL);

	ZSTD_DStream *zdctx = vy_env_get_zdctx(run->env);
	if (zdctx == NULL)
		return -1;

	struct vy_page_info *page_info =
		vy_compaction_stream_page_info(stream, stream->page_no);
	if (page_info == NULL)
		return -1;

	stream->page = vy_page_new(page_info);
	if (stream->page == NULL)
		return -1;

	if (vy_page_read(stream->page, page_info, run, zdctx) != 0) {
		vy_page_delete(stream->page);
		stream->page = NULL;
		return -1;
	}
	return 0;
}

static int
vy_compaction_stream_search(struct vy_stmt_stream *virt_stream)
{
	struct vy_compaction_stream *stream =
		(struct vy_compaction_stream *)virt_stream;
	assert(stream->page == NULL);
	if (stream->slice->begin.stmt == NULL) {
		assert(stream->page_no == 0);
		assert(stream->pos_in_page == 0);
		return 0;
	}

	if (vy_compaction_stream_read_page(stream) != 0)
		return -1;

	bool unused;
	if (vy_page_find_key(stream->page, stream->slice->begin,
			     stream->cmp_def, stream->format, ITER_GE,
			     &stream->pos_in_page, &unused) != 0) {
		vy_page_delete(stream->page);
		stream->page = NULL;
		return -1;
	}

	if (stream->pos_in_page == stream->page->row_count) {
		vy_page_delete(stream->page);
		stream->page = NULL;
		stream->page_no++;
		stream->pos_in_page = 0;
	}
	return 0;
}

static NODISCARD int
vy_compaction_stream_next(struct vy_stmt_stream *virt_stream,
			  struct vy_entry *ret)
{
	struct vy_compaction_stream *stream =
		(struct vy_compaction_stream *)virt_stream;

	if (stream->page_no > stream->slice->last_page_no) {
		*ret = vy_entry_none();
		return 0;
	}

	if (stream->page == NULL &&
	    vy_compaction_stream_read_page(stream) != 0)
		return -1;

	struct vy_entry entry = vy_page_stmt(stream->page, stream->pos_in_page,
					     stream->cmp_def, stream->format);
	if (entry.stmt == NULL)
		return -1;

	/* Check that the tuple is not out of slice bounds = */
	if (stream->page_no >= stream->slice->last_page_no &&
	    vy_bound_cmp(entry, stream->slice->end_bound,
			 stream->cmp_def) > 0) {
		tuple_unref(entry.stmt);
		*ret = vy_entry_none();
		return 0;
	}

	if (stream->entry.stmt != NULL)
		tuple_unref(stream->entry.stmt);
	stream->entry = entry;
	*ret = entry;

	stream->pos_in_page++;

	struct vy_page_info *page_info =
		vy_compaction_stream_page_info(stream, stream->page_no);
	if (page_info == NULL)
		return -1;
	if (stream->pos_in_page >= page_info->row_count) {
		vy_page_delete(stream->page);
		stream->page = NULL;
		stream->page_no++;
		stream->pos_in_page = 0;
	}

	return 0;
}

static void
vy_compaction_stream_stop(struct vy_stmt_stream *virt_stream)
{
	struct vy_compaction_stream *stream =
		(struct vy_compaction_stream *)virt_stream;
	if (stream->page != NULL) {
		vy_page_delete(stream->page);
		stream->page = NULL;
	}
	if (stream->entry.stmt != NULL) {
		tuple_unref(stream->entry.stmt);
		stream->entry = vy_entry_none();
	}
	vy_compaction_stream_free_block(stream);
}

static void
vy_compaction_stream_close(struct vy_stmt_stream *virt_stream)
{
	struct vy_compaction_stream *stream =
		(struct vy_compaction_stream *)virt_stream;
	tuple_format_unref(stream->format);
}

static const struct vy_stmt_stream_iface vy_compaction_stream_iface = {
	.start = vy_compaction_stream_search,
	.next = vy_compaction_stream_next,
	.stop = vy_compaction_stream_stop,
	.close = vy_compaction_stream_close,
};

void
vy_compaction_stream_open(struct vy_compaction_stream *stream,
			  struct vy_slice *slice, struct key_def *cmp_def,
			  struct tuple_format *format)
{
	stream->base.iface = &vy_compaction_stream_iface;
	stream->page_no = slice->first_page_no;
	stream->pos_in_page = 0;
	stream->page = NULL;
	stream->entry = vy_entry_none();
	stream->block_pages = NULL;
	stream->block_page_count = 0;
	stream->block_first_page_no = 0;
	stream->slice = slice;
	stream->cmp_def = cmp_def;
	stream->format = format;
	tuple_format_ref(format);
}
