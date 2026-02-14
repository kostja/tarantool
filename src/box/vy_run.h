#ifndef INCLUDES_TARANTOOL_BOX_VY_RUN_H
#define INCLUDES_TARANTOOL_BOX_VY_RUN_H
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

#include <stdint.h>
#include <stdbool.h>

#include "fiber_cond.h"
#include "iterator_type.h"
#include "vy_entry.h"
#include "vy_stmt_stream.h"
#include "vy_read_view.h"
#include "vy_stat.h"
#include "index_def.h"
#include "xlog.h"

#include "small/mempool.h"

#include "vy_index_cache.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_history;
struct vy_run_reader;

/**
 * Default memory quota for the page index cache (128 MB).
 */
enum { VY_INDEX_CACHE_DEFAULT_SIZE = 128 * 1024 * 1024 };

/** Part of vinyl environment for run read/write */
struct vy_run_env {
	/** Write rate limit, in bytes per second. */
	uint64_t snap_io_rate_limit;
	/** Mempool for struct vy_page_read_task */
	struct mempool read_task_pool;
	/** Key for thread-local ZSTD context */
	pthread_key_t zdctx_key;
	/** Pool of threads used for reading run files. */
	struct vy_run_reader *reader_pool;
	/** Tuple format to create vy_stmt from run min/max key. */
	struct tuple_format *key_format;
	/** Number of threads in the reader pool. */
	int reader_pool_size;
	/**
	 * Index of the reader thread in the pool to be used for
	 * processing the next read request.
	 */
	int next_reader;
	/**
	 * Use vinyl-local random seed to minimize impact of rand()
	 * calls between different subsystems.
	 */
	unsigned int seed;
	/**
	 * We need this flag during compaction in order to determine we can
	 * unconditionally remove unused runs' files in-place.
	 */
	bool initial_join;
	/**
	 * 2Q cache for .index2 index blocks.
	 * Entries are decoded vy_page_info arrays keyed by
	 * (run_id, block_no).
	 */
	struct vy_index_cache index_cache;
};

/**
 * Run metadata. Is a written to a file as a single chunk.
 */
struct vy_run_info {
	/** Min key in the run. */
	char *min_key;
	/** Max key in the run. */
	char *max_key;
	/** Min LSN over all statements in the run. */
	int64_t min_lsn;
	/** Max LSN over all statements in the run. */
	int64_t max_lsn;
	/** Number of pages in the run. */
	uint32_t page_count;
	/** Statement statistics. */
	struct vy_stmt_stat stmt_stat;
	/**
	 * MinHash sketch for the entire run.
	 * Computed as the merge of all per-block sketches.
	 * Used for overlap estimation in compaction planning.
	 */
	struct minhash sketch;
	/** True if the run-level sketch is available. */
	bool has_sketch;
};

/**
 * Run page metadata. Is a written to a file as a single chunk.
 */
struct vy_page_info {
	/** Offset of page data in the run file. */
	uint64_t offset;
	/** Minimal key stored in the page. */
	char *min_key;
	/** Comparison hint of the min key. */
	hint_t min_key_hint;
	/** Size of page data in the run file. */
	uint32_t size;
	/** Size of page data in memory, i.e. unpacked. */
	uint32_t unpacked_size;
	/** Number of statements in the page. */
	uint32_t row_count;
	/** Offset of the row index in the page. */
	uint32_t row_index_offset;
};

/**
 * Number of data pages described by a single index block
 * in the .index2 file.  Chosen so that an index block is
 * typically a few KB, which is a good unit for caching and
 * I/O.  Must be a power of two for fast division.
 */
enum { VY_INDEX_BLOCK_SIZE = 64 };

/**
 * Block directory entry for the .index2 format.
 *
 * The block directory is stored in the run_info header row
 * of the .index2 file and is always kept in RAM.  Each entry
 * describes one index block — a group of VY_INDEX_BLOCK_SIZE
 * page_info entries.
 */
struct vy_index_block_dir {
	/**
	 * Min key of the first page in this block.
	 * Used for binary search when looking up a page.
	 */
	char *boundary_key;
	hint_t boundary_key_hint;
	/**
	 * Byte offset of this block's xlog transaction in the
	 * .index2 file.  Filled in at recovery time by scanning
	 * the file sequentially and recording xlog_cursor_pos()
	 * for each block row.
	 */
	uint64_t file_offset;
	/**
	 * Size in bytes of the xlog transaction for this block,
	 * including the fixheader.  Used by demand-loading to
	 * know how many bytes to pread from index_fd.
	 * Filled in at recovery time.
	 */
	uint32_t data_size;
	/** Number of page_info entries in this block. */
	uint32_t page_count;
	/** Global page number of the first page in this block. */
	uint32_t first_page_no;
	/**
	 * In-memory copy of this block's fuse8 filter (if any).
	 * Kept in RAM so that bloom checks can be performed
	 * without yielding to load the index block from disk.
	 */
	binary_fuse8_t filter;
	/** True if @a filter was successfully loaded. */
	bool has_filter;
};

/**
 * Logical unit of vinyl index - a sorted file with data.
 */
struct vy_run {
	/** Vinyl run environment. */
	struct vy_run_env *env;
	/** Info about the run stored in the index file. */
	struct vy_run_info info;
	/** Info about the run pages stored in the index file. */
	struct vy_page_info *page_info;
	/** Run data file. */
	int fd;
	/** Unique ID of this run. */
	int64_t id;
	/** Number of statements in this run. */
	struct vy_disk_stmt_counter count;
	/** Size of memory used for storing page index. */
	size_t page_index_size;
	/** Max LSN stored on disk. */
	int64_t dump_lsn;
	/**
	 * Number of dumps it took to create this run.
	 *
	 * If the run was produced by a memory dump, it is 1.
	 * If the run was produced by a minor compaction, it
	 * is is the sum of dump counts of compacted runs.
	 * If the run was produced by a major compaction, it
	 * is is the sum of dump counts of compacted runs
	 * minus the dump count of the last (greatest) run.
	 *
	 * This way, by looking at the last level run in an LSM
	 * tree, we can tell how many dumps it took to compact
	 * it last time.
	 */
	uint32_t dump_count;
	/**
	 * Run reference counter, the run is deleted once it hits 0.
	 * A new run is created with the reference counter set to 1.
	 * A run is referenced by each slice created for it and each
	 * pending read or write task.
	 */
	int refs;
	/** Number of slices created for this run. */
	int slice_count;
	/**
	 * Total pages referenced by all live slices of this run.
	 * Incremented in vy_range_add_slice (via vy_range_init_slice),
	 * decremented in vy_range_remove_slice.  Used to compute the
	 * unreferenced portion of the run file for bloat detection.
	 * Page counts are exact (no rounding), unlike byte estimates.
	 */
	uint32_t referenced_pages;
	/**
	 * Counter used on completion of a compaction task to check if
	 * all slices of the run have been compacted and so the run is
	 * not used any more and should be deleted.
	 */
	int compacted_slice_count;
	/**
	 * Link in the list of runs that became unused
	 * after compaction.
	 */
	struct rlist in_unused;
	/** Link in vy_lsm::runs list. */
	struct rlist in_lsm;
	/**
	 * v2 index format (.index2): block directory.
	 *
	 * When the run has been recovered from a .index2 file,
	 * page_info is NULL and the block directory is used
	 * instead.  Individual page_info blocks are loaded on
	 * demand through the index cache.
	 *
	 * When block_dir is NULL, the run uses the v1 format
	 * with page_info loaded entirely in memory.
	 */
	struct vy_index_block_dir *block_dir;
	/** Number of entries in block_dir. */
	uint32_t block_count;
	/**
	 * File descriptor for the .index2 file, kept open for
	 * demand-loading index blocks via pread.  Set to -1 if
	 * not available (v1 runs).
	 */
	int index_fd;
};

/**
 * Slice of a run, used to organize runs in ranges.
 */
struct vy_slice {
	/** Unique ID of this slice. */
	int64_t id;
	/** Run this slice is for (increments vy_run::refs). */
	struct vy_run *run;
	/**
	 * Slice begin (increments tuple::refs).
	 */
	struct vy_entry begin;
	/**
	 * Tightest upper bound on keys stored in this slice
	 * (increments tuple::refs).
	 *
	 * An inclusive key (max_key) when the run's max key falls
	 * within this range (typical case, non-shared runs).
	 * An exclusive key (with VY_STMT_EXCLUSIVE_BOUND flag)
	 * when the run extends past this range's boundary
	 * (shared run clipped by range split).
	 */
	struct vy_entry end_bound;
	/**
	 * Random seed used for compaction randomization.
	 * Lays in range [0, RAND_MAX].
	 */
	int seed;
	/**
	 * Number of async users of this slice. Slice must not
	 * be removed until it hits 0. Used by the iterator to
	 * prevent use-after-free after waiting for IO.
	 * See also vy_run_wait_pinned().
	 */
	int pin_count;
	/**
	 * Condition variable signaled by vy_slice_unpin()
	 * if pin_count reaches 0.
	 */
	struct fiber_cond pin_cond;
	union {
		/** Link in range->slices list. */
		struct rlist in_range;
		/** Link in vy_join_ctx->slices list. */
		struct rlist in_join;
	};
	/**
	 * Indexes of the first and the last page in the run
	 * that belong to this slice.
	 */
	uint32_t first_page_no;
	uint32_t last_page_no;
	/** An estimate of the number of statements in this slice. */
	struct vy_disk_stmt_counter count;
};

/** Position of a particular stmt in vy_run. */
struct vy_run_iterator_pos {
	uint32_t page_no;
	uint32_t pos_in_page;
};

/**
 * Return statements from vy_run based on initial search key,
 * iteration order and view lsn.
 *
 * All statements with lsn > vlsn are skipped.
 * The API allows to traverse over resulting statements within two
 * dimensions - key and lsn. next_key() switches to the youngest
 * statement of the next key, according to the iteration order,
 * and next_lsn() switches to an older statement for the same
 * key.
 */
struct vy_run_iterator {
	/** Usage statistics */
	struct vy_run_iterator_stat *stat;

	/* Members needed for memory allocation and disk access */
	/** Key definition used for comparing statements on disk. */
	struct key_def *cmp_def;
	/** Key definition provided by the user. */
	struct key_def *key_def;
	/**
	 * Format ot allocate REPLACE and DELETE tuples read from
	 * pages.
	 */
	struct tuple_format *format;
	/** The run slice to iterate. */
	struct vy_slice *slice;

	/* Search options */
	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if the key is not specified, GT and EQ are changed to
	 * GE, LT to LE for beauty.
	 */
	enum iterator_type iterator_type;
	/** Key to search. */
	struct vy_entry key;
	/* LSN visibility, iterator shows values with lsn <= vlsn */
	const struct vy_read_view **read_view;

	/* State of the iterator */
	/** Position of the current record */
	struct vy_run_iterator_pos curr_pos;
	/** Statement at curr_pos. */
	struct vy_entry curr;
	/**
	 * Last two pages read by the iterator. We keep two pages
	 * rather than just one, because we often probe a page for
	 * a better match. Keeping the previous page makes sure we
	 * won't throw out the current page if probing fails to
	 * find a better match.
	 */
	struct vy_page *curr_page;
	struct vy_page *prev_page;
	/** Is false until first .._get or .._next_.. method is called */
	bool search_started;
};

/**
 * Vinyl page stored in memory.
 */
struct vy_page {
	/** Page position in the run file. */
	uint32_t page_no;
	/** Size of page data in memory, i.e. unpacked. */
	uint32_t unpacked_size;
	/** Number of statements in the page. */
	uint32_t row_count;
	/** Array of row offsets. */
	uint32_t *row_index;
	/** Pointer to the page data. */
	char *data;
};

/**
 * Level-1 binary search in the block directory.
 * Returns the index of the block whose boundary key best matches
 * @a key for the given @a itype.  Cannot fail (block_dir is always
 * in memory).
 *
 * For forward iterators (GE/GT): returns the last block whose
 * boundary_key < key (lower_bound) or boundary_key <= key
 * (upper_bound), or 0 if key precedes all blocks.
 *
 * For reverse iterators (LE/LT): returns the last block whose
 * boundary_key <= key (upper_bound) or boundary_key < key
 * (lower_bound), or 0 if key precedes all blocks.
 */
uint32_t
vy_block_dir_find_block(struct vy_run *run, struct vy_entry key,
			struct key_def *cmp_def, enum iterator_type itype);

/**
 * Decode block directory from msgpack.
 *
 * Unknown fields in each directory entry are silently skipped
 * for forward compatibility.
 */
int
vy_block_dir_decode(struct vy_run *run, const char **data,
		    struct key_def *cmp_def, const char *filename);

/**
 * Decode a page_info entry from an xrow.
 *
 * Unknown keys in the map are silently skipped for forward
 * compatibility.
 */
int
vy_page_info_decode(struct vy_page_info *page, const struct xrow_header *xrow,
		    struct key_def *cmp_def, const char *filename);

/**
 * Decode pages from a msgpack array of page_info maps.
 *
 * The returned @a pages array is heap-allocated; the caller must
 * free it and each page's min_key.
 *
 * @param[out] mem_out  If non-NULL, total memory consumed.
 */
int
vy_index_block_decode_pages(const char *data, struct key_def *cmp_def,
			     struct vy_page_info **pages_out,
			     uint32_t *count_out, size_t *mem_out);

/**
 * Find a page from which the iteration of a given key must be started.
 * LE and LT: the found page definitely contains the position
 *  for iteration start.
 * GE, GT, EQ: Since page search uses only min_key of pages,
 *  it may happen that the found page doesn't contain the position
 *  for iteration start. In this case it is certain that the iteration
 *  must be started from the beginning of the next page.
 *
 * @param run - run
 * @param key - key to find
 * @param key_def - key_def for comparison
 * @param itype - iterator type (see above)
 * @param equal_key: *equal_key is set to true if there is a page
 *  with min_key equal to the given key.
 * @param[out] result - page number, or run->info.page_count if
 *  there are no pages fulfilling the conditions.
 * @retval  0 success
 * @retval -1 error (diag is set)
 */
int
vy_page_index_find_page(struct vy_run *run, struct vy_entry key,
			struct key_def *cmp_def, enum iterator_type itype,
			bool *equal_key, uint32_t *result);
/**
 * Initialize vinyl run environment
 *
 * @param key_format - plain format for run min/max keys
 * @param read_threads - max number of background threads to
 * use for disk reads; note background threads are not used
 * until vy_run_env_enable_coio() is called.
 */
void
vy_run_env_create(struct vy_run_env *env, struct tuple_format *key_format,
		  int read_threads);

/**
 * Destroy vinyl run environment
 */
void
vy_run_env_destroy(struct vy_run_env *env);

/**
 * Enable coio reads for a vinyl run environment.
 *
 * This function starts background reader threads and makes
 * the run iterator hand disk reads over to them rather
 * than read run files directly blocking the current fiber.
 *
 * The number of background reader threads is configured when
 * the environment is created, see vy_run_env_create().
 *
 * Subsequent calls to this function will silently return.
 */
void
vy_run_env_enable_coio(struct vy_run_env *env);

static inline struct vy_page_info *
vy_run_page_info(struct vy_run *run, uint32_t pos)
{
	assert(pos < run->info.page_count);
	assert(run->page_info != NULL);
	return &run->page_info[pos];
}

/**
 * Get page_info for a v2 run via the index block cache.
 *
 * For v2 runs where page_info is not eagerly loaded (page_info == NULL),
 * this function demand-loads the appropriate index block through the
 * 2Q cache and returns a pointer to the requested page_info entry.
 *
 * The returned pointer is valid until the cache entry is evicted.
 * Must be called from TX thread (may yield for coio).
 *
 * @param run     The run.
 * @param pos     Global page number.
 * @param cmp_def Key definition for decoding.
 * @return Pointer to page_info, or NULL on error.
 */
struct vy_page_info *
vy_run_page_info_v2(struct vy_run *run, uint32_t pos,
		    struct key_def *cmp_def);

static inline bool
vy_run_is_empty(struct vy_run *run)
{
	return run->info.page_count == 0;
}

struct vy_run *
vy_run_new(struct vy_run_env *env, int64_t id);

void
vy_run_delete(struct vy_run *run);

static inline void
vy_run_ref(struct vy_run *run)
{
	assert(run->refs > 0);
	run->refs++;
}

static inline void
vy_run_unref(struct vy_run *run)
{
	assert(run->refs > 0);
	if (--run->refs == 0)
		vy_run_delete(run);
}

/**
 * Load run from disk.
 *
 * First tries to load the .index2 (v2 format).  If the file
 * does not exist, rebuilds the index by scanning the .run data
 * file and writes a new .index2.
 *
 * @param run - run to load
 * @param dir - path to the vinyl directory
 * @param space_id - space id
 * @param iid - index id
 * @param cmp_def - definition of keys stored in the run
 * @param format - format for decoding tuples (needed for rebuild)
 * @return - 0 on success, -1 on fail
 */
int
vy_run_recover(struct vy_run *run, const char *dir,
	       uint32_t space_id, uint32_t iid, struct key_def *cmp_def,
	       struct tuple_format *format);

/**
 * Rebuild run index
 * @param run - run to rebuild index for
 * @param dir - path to the vinyl directory
 * @param space_id - space id
 * @param iid - index id
 * @param cmp_def - key definition with primary key parts
 * @param key_def - user defined key definition
 * @param format - format for allocating tuples read from disk
 * @param opts - index options
 * @return - 0 on sucess, -1 on fail
 */
int
vy_run_rebuild_index(struct vy_run *run, const char *dir,
		     uint32_t space_id, uint32_t iid,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct tuple_format *format,
		     const struct index_opts *opts);

enum vy_file_type {
	/**
	 * Legacy v1 index files.  No longer created or read,
	 * but kept so that vy_run_remove_files() can clean up
	 * old files from upgraded installations.
	 */
	VY_FILE_INDEX,
	VY_FILE_INDEX_INPROGRESS,
	VY_FILE_INDEX2,
	VY_FILE_INDEX2_INPROGRESS,
	VY_FILE_RUN,
	VY_FILE_RUN_INPROGRESS,
	vy_file_MAX,
};

extern const char *vy_file_suffix[];

static inline int
vy_space_snprint_path(char *buf, int size, const char *dir,
		      uint32_t space_id)
{
	return snprintf(buf, size, "%s/%u", dir, (unsigned)space_id);
}

static inline int
vy_lsm_snprint_path(char *buf, int size, const char *dir,
		    uint32_t space_id, uint32_t iid)
{
	int total = 0;
	SNPRINT(total, vy_space_snprint_path, buf, size, dir,
		(unsigned)space_id);
	SNPRINT(total, snprintf, buf, size, "/%u", (unsigned)iid);
	return total;
}

static inline int
vy_run_snprint_filename(char *buf, int size, int64_t run_id,
			enum vy_file_type type)
{
	return snprintf(buf, size, "%020lld.%s",
			(long long)run_id, vy_file_suffix[type]);
}

static inline int
vy_run_snprint_path(char *buf, int size, const char *dir,
		    uint32_t space_id, uint32_t iid,
		    int64_t run_id, enum vy_file_type type)
{
	int total = 0;
	SNPRINT(total, vy_lsm_snprint_path, buf, size,
		dir, (unsigned)space_id, (unsigned)iid);
	SNPRINT(total, snprintf, buf, size, "/");
	SNPRINT(total, vy_run_snprint_filename, buf, size, run_id, type);
	return total;
}

/**
 * Remove all files (data, index) corresponding to a run
 * with the given id. Return 0 on success, -1 if unlink()
 * failed.
 */
int
vy_run_remove_files(const char *dir, uint32_t space_id,
		    uint32_t iid, int64_t run_id);

/**
 * Allocate a new run slice.
 * This function increments @run->refs.
 */
struct vy_slice *
vy_slice_new(int64_t id, struct vy_run *run);

/**
 * Free a run slice.
 * This function decrements @run->refs and
 * deletes the run if the counter hits 0.
 */
void
vy_slice_delete(struct vy_slice *slice);

/**
 * Pin a run slice.
 * A pinned slice can't be deleted until it's unpinned.
 */
static inline void
vy_slice_pin(struct vy_slice *slice)
{
	slice->pin_count++;
}

/*
 * Unpin a run slice.
 * This function reverts the effect of vy_slice_pin().
 */
static inline void
vy_slice_unpin(struct vy_slice *slice)
{
	assert(slice->pin_count > 0);
	if (--slice->pin_count == 0)
		fiber_cond_broadcast(&slice->pin_cond);
}

/**
 * Wait until a run slice is unpinned.
 */
static inline void
vy_slice_wait_pinned(struct vy_slice *slice)
{
	while (slice->pin_count > 0)
		fiber_cond_wait(&slice->pin_cond);
}

/**
 * Cut a sub-slice of @slice starting at @begin and ending at @end.
 * Return 0 on success, -1 on OOM.
 *
 * The new slice is returned in @result. If @slice does not intersect
 * with [@begin, @end), @result is set to NULL.
 */
int
vy_slice_cut(struct vy_slice *slice, int64_t id, struct vy_entry begin,
	     struct vy_entry end, struct key_def *cmp_def,
	     struct vy_slice **result);

/**
 * Open an iterator over on-disk run.
 *
 * Note, it is the caller's responsibility to make sure the slice
 * is not compacted while the iterator is reading it.
 */
void
vy_run_iterator_open(struct vy_run_iterator *itr,
		     struct vy_run_iterator_stat *stat,
		     struct vy_slice *slice, enum iterator_type iterator_type,
		     struct vy_entry key, const struct vy_read_view **rv,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct tuple_format *format);

/**
 * Advance a run iterator to the next key.
 * The key history is returned in @history (empty if EOF).
 * Returns 0 on success, -1 on memory allocation or IO error.
 */
NODISCARD int
vy_run_iterator_next(struct vy_run_iterator *itr,
		     struct vy_history *history);

/**
 * Advance a run iterator to the key following @last.
 * The key history is returned in @history (empty if EOF).
 * Returns 0 on success, -1 on memory allocation or IO error.
 */
NODISCARD int
vy_run_iterator_skip(struct vy_run_iterator *itr, struct vy_entry last,
		     struct vy_history *history);

/**
 * Close a run iterator.
 */
void
vy_run_iterator_close(struct vy_run_iterator *itr);

/**
 * Stream for reading tuples from a run slice during compaction.
 *
 * Reads pages sequentially from a slice, loading index blocks
 * from the .index2 file via blocking pread.  Designed to run
 * entirely on a compaction worker thread with no dependency on
 * the TX thread or the index cache.
 */
struct vy_compaction_stream {
	/** Parent class, must be the first member. */
	struct vy_stmt_stream base;
	/** Current global page number. */
	uint32_t page_no;
	/** Current position in the page. */
	uint32_t pos_in_page;
	/** Last page read from disk. */
	struct vy_page *page;
	/** The last tuple returned to user. */
	struct vy_entry entry;
	/** Slice to stream. */
	struct vy_slice *slice;
	/** Key def for comparisons. */
	struct key_def *cmp_def;
	/** Format for allocating tuples. */
	struct tuple_format *format;
	/**
	 * Locally cached decoded index block, loaded from
	 * .index2 via blocking pread.
	 */
	struct vy_page_info *block_pages;
	/** Number of pages in the cached block. */
	uint32_t block_page_count;
	/** First global page number of the cached block. */
	uint32_t block_first_page_no;
};

/**
 * Open a compaction stream for a v2 run.
 * Use vy_stmt_stream api for further work.
 */
void
vy_compaction_stream_open(struct vy_compaction_stream *stream,
			  struct vy_slice *slice, struct key_def *cmp_def,
			  struct tuple_format *format);

/**
 * Run_writer fills a created run with statements one by one,
 * splitting them into pages.
 */
/**
 * Probabilistic data structures (PDS) for one completed
 * index block, built during the run write.
 */
struct vy_block_pds {
	/** Binary fuse8 membership filter. */
	binary_fuse8_t filter;
	/** True if the filter was successfully built. */
	bool has_filter;
	/** MinHash overlap sketch. */
	struct minhash sketch;
};

struct vy_run_writer {
	/** Run to fill. */
	struct vy_run *run;
	/** Path to directory with run files. */
	const char *dirpath;
	/** Identifier of a space owning the run. */
	uint32_t space_id;
	/** Identifier of an index owning the run. */
	uint32_t iid;
	/**
	 * Key definition to extract from tuple and store as page
	 * min key, run min/max keys, and secondary index
	 * statements.
	 */
	struct key_def *cmp_def;
	/** Key definition for hashing (used for fuse filters). */
	struct key_def *key_def;
	/** Various options, e.g. minimal page size. */
	struct index_opts index_opts;
	/**
	 * Current page info capacity. Can grow with page number.
	 */
	uint32_t page_info_capacity;
	/** Xlog to write data. */
	struct xlog data_xlog;
	/** Buffer of a current page row offsets. */
	struct ibuf row_index_buf;
	/**
	 * Remember a last written statement to use it as a source
	 * of max key of a finished run.
	 */
	struct vy_entry last;

	/*
	 * Per-block probabilistic data structures.
	 *
	 * As pages are written, we accumulate 64-bit key hashes
	 * and update the MinHash sketch for the current block.
	 * When a block boundary is crossed (every VY_INDEX_BLOCK_SIZE
	 * pages), the fuse filter is built from the accumulated
	 * hashes, and the block PDS is finalized.
	 */

	/** Accumulator of 64-bit key hashes for the current block. */
	uint64_t *block_hashes;
	/** Number of hashes accumulated. */
	uint32_t block_hash_count;
	/** Capacity of the block_hashes array. */
	uint32_t block_hash_cap;
	/** MinHash sketch for the current block. */
	struct minhash block_sketch;

	/** Array of finalized PDS for completed blocks. */
	struct vy_block_pds *block_pds;
	/** Number of finalized blocks. */
	uint32_t block_pds_count;
	/** Capacity of the block_pds array. */
	uint32_t block_pds_cap;
};

/** Create a run writer to fill a run with statements. */
int
vy_run_writer_create(struct vy_run_writer *writer, struct vy_run *run,
		     const char *dirpath, uint32_t space_id, uint32_t iid,
		     struct key_def *cmp_def, struct key_def *key_def,
		     struct index_opts *index_opts);

/**
 * Write a specified statement into a run.
 * @param writer Writer to write a statement.
 * @param entry Statement to write.
 *
 * @retval -1 Memory error.
 * @retval  0 Success.
 */
int
vy_run_writer_append_stmt(struct vy_run_writer *writer, struct vy_entry entry);

/**
 * Finalize run writing by writing run index into file. The writer
 * is deleted after call.
 * @param writer Run writer.
 * @retval -1 Memory or IO error.
 * @retval  0 Success.
 */
int
vy_run_writer_commit(struct vy_run_writer *writer);

/**
 * Abort run writing. Can not delete a run and run's file here,
 * becase it must be done from tx thread. The writer is deleted
 * after call.
 * @param Run writer.
 */
void
vy_run_writer_abort(struct vy_run_writer *writer);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_RUN_H */
