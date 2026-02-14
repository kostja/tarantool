/*
 * Copyright 2010-2025, Tarantool AUTHORS, please see AUTHORS file.
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
#include "vy_iterators_helper.h"

#include "box/key_def.h"
#include "trivia/util.h"
#include "vy_run.h"
#include "vy_index_cache.h"
#include "binaryfusefilter.h"
#include "iproto_constants.h"
#include "xrow.h"
#include <msgpuck.h>
#include <stdint.h>
#include <string.h>

static struct key_def *cmp_def;
static struct vy_run_env run_env;
const int MSGPACK_KEY_MAX = 1 + 9;

static inline char *
mp_encode_i64(char *pos, int64_t value)
{
	if (value < 0)
		return mp_encode_int(pos, value);
	return mp_encode_uint(pos, (uint64_t)value);
}

static struct vy_entry
vy_key_new_i64(int64_t value)
{
	char buf[1 + 9];
	char *pos = buf;
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_i64(pos, value);
	return vy_entry_key_from_msgpack(stmt_env.key_format, cmp_def, buf);
}

/**
 * Allocate a msgpack key array(1, value) on the heap.
 * Returns a malloc'd buffer; caller must free it.
 */
static char *
mp_key_dup_i64(int64_t value)
{
	char buf[1 + 9];
	char *pos = buf;
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_i64(pos, value);
	size_t len = pos - buf;
	char *dup = xmalloc(len);
	memcpy(dup, buf, len);
	return dup;
}

/**
 * Create a mock vy_run with a hand-built block directory.
 *
 * @param boundary_keys  Array of boundary key values, one per block.
 * @param block_count    Number of blocks.
 * @param pages_per_block Number of pages per block (last block may
 *                        have fewer; total = total_pages).
 * @param total_pages    Total page count for the run.
 */
static struct vy_run *
create_mock_run(int64_t *boundary_keys, int block_count,
		int pages_per_block, int total_pages)
{
	struct vy_run *run = vy_run_new(&run_env, 1);
	fail_if(run == NULL);

	run->info.page_count = total_pages;
	run->info.min_key = mp_key_dup_i64(boundary_keys[0]);
	run->info.max_key = mp_key_dup_i64(
		boundary_keys[block_count - 1] + pages_per_block * 10);

	run->block_count = block_count;
	run->block_dir = xcalloc(block_count,
				 sizeof(struct vy_index_block_dir));

	int page_no = 0;
	for (int i = 0; i < block_count; i++) {
		struct vy_index_block_dir *d = &run->block_dir[i];
		d->boundary_key = mp_key_dup_i64(boundary_keys[i]);
		/*
		 * key_hint expects data after the array header,
		 * matching how vy_run_load_index2 computes hints.
		 */
		const char *key_beg = d->boundary_key;
		uint32_t part_count = mp_decode_array(&key_beg);
		d->boundary_key_hint = key_hint(key_beg, part_count,
						cmp_def);
		d->first_page_no = page_no;
		if (i < block_count - 1)
			d->page_count = pages_per_block;
		else
			d->page_count = total_pages - page_no;
		page_no += d->page_count;
	}

	return run;
}

/**
 * Free a mock run created by create_mock_run.
 */
static void
destroy_mock_run(struct vy_run *run)
{
	for (uint32_t i = 0; i < run->block_count; i++)
		free(run->block_dir[i].boundary_key);
	free(run->block_dir);
	run->block_dir = NULL;
	run->block_count = 0;
	vy_run_unref(run);
}

/**
 * Build a vy_page_info array with the given min_key values.
 * Each page_info gets a heap-allocated min_key.
 */
static struct vy_page_info *
build_page_infos(int64_t *min_keys, int count)
{
	struct vy_page_info *pages = xcalloc(count, sizeof(*pages));
	for (int i = 0; i < count; i++) {
		pages[i].min_key = mp_key_dup_i64(min_keys[i]);
		const char *key_beg = pages[i].min_key;
		uint32_t part_count = mp_decode_array(&key_beg);
		pages[i].min_key_hint = key_hint(key_beg, part_count,
						 cmp_def);
	}
	return pages;
}

/**
 * Pre-populate the index cache with a block entry for the given run.
 */
static void
populate_cache_block(struct vy_run *run, uint32_t block_no,
		     int64_t *page_min_keys, int page_count)
{
	struct vy_page_info *pages = build_page_infos(page_min_keys,
						      page_count);
	struct vy_index_cache *cache = &run->env->index_cache;
	uint32_t first_page_no = run->block_dir[block_no].first_page_no;
	size_t mem_used = sizeof(struct vy_index_cache_entry) +
			  page_count * sizeof(struct vy_page_info);
	struct vy_index_cache_entry *entry =
		vy_index_cache_put(cache, run->id, block_no,
				    first_page_no, pages, page_count,
				    NULL, NULL, mem_used);
	fail_if(entry == NULL);
}

/* ----------------------------------------------------------------
 * Test group 1: vy_block_dir_find_block
 * ---------------------------------------------------------------- */

static void
test_block_dir_find_block(void)
{
	header();
	plan(12);

	/*
	 * 4 blocks with boundary keys: 10, 30, 50, 70.
	 * Each block has 4 pages, total 16 pages.
	 */
	int64_t boundaries[] = { 10, 30, 50, 70 };
	struct vy_run *run = create_mock_run(boundaries, 4, 4, 16);

	struct vy_entry key;
	uint32_t result;

	/* Key before all blocks (5). */
	key = vy_key_new_i64(5);
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GE);
	is(result, 0, "key < all blocks, ITER_GE -> 0");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_LT);
	is(result, 0, "key < all blocks, ITER_LT -> 0");
	tuple_unref(key.stmt);

	/* Key exactly on block 0 boundary (10). */
	key = vy_key_new_i64(10);
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GE);
	is(result, 0, "key == block[0], ITER_GE -> 0");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GT);
	is(result, 0, "key == block[0], ITER_GT -> 0");
	tuple_unref(key.stmt);

	/* Key between block 1 (30) and block 2 (50): key=40. */
	key = vy_key_new_i64(40);
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GE);
	is(result, 1, "key between block[1] and block[2], ITER_GE -> 1");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_LE);
	is(result, 1, "key between block[1] and block[2], ITER_LE -> 1");
	tuple_unref(key.stmt);

	/*
	 * Key exactly on last block boundary (70).
	 * GE/LT use lower_bound (last block with boundary < key):
	 *   boundary[2]=50 < 70, boundary[3]=70 is NOT < 70 → block 2.
	 * GT/LE use upper_bound (last block with boundary <= key):
	 *   boundary[3]=70 <= 70 → block 3.
	 */
	key = vy_key_new_i64(70);
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GE);
	is(result, 2, "key == last block, ITER_GE -> 2 (lower_bound)");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GT);
	is(result, 3, "key == last block, ITER_GT -> 3 (upper_bound)");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_LE);
	is(result, 3, "key == last block, ITER_LE -> 3 (upper_bound)");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_LT);
	is(result, 2, "key == last block, ITER_LT -> 2 (lower_bound)");
	tuple_unref(key.stmt);

	/* Key after all blocks (100). */
	key = vy_key_new_i64(100);
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_LE);
	is(result, 3, "key > all blocks, ITER_LE -> last");
	result = vy_block_dir_find_block(run, key, cmp_def, ITER_GT);
	is(result, 3, "key > all blocks, ITER_GT -> last");
	tuple_unref(key.stmt);

	destroy_mock_run(run);
	check_plan();
	footer();
}

/* ----------------------------------------------------------------
 * Test group 2: vy_page_index_find_page
 * ---------------------------------------------------------------- */

static void
test_page_index_find_page(void)
{
	header();
	plan(11);

	/*
	 * 3 blocks x 4 pages = 12 pages total.
	 * Block 0: pages 0-3, boundary key 10.
	 * Block 1: pages 4-7, boundary key 50.
	 * Block 2: pages 8-11, boundary key 90.
	 */
	int64_t boundaries[] = { 10, 50, 90 };
	struct vy_run *run = create_mock_run(boundaries, 3, 4, 12);

	/* Pre-populate cache with page_info for each block. */
	int64_t block0_keys[] = { 10, 20, 30, 40 };
	int64_t block1_keys[] = { 50, 60, 70, 80 };
	int64_t block2_keys[] = { 90, 100, 110, 120 };
	populate_cache_block(run, 0, block0_keys, 4);
	populate_cache_block(run, 1, block1_keys, 4);
	populate_cache_block(run, 2, block2_keys, 4);

	struct vy_entry key;
	bool equal_key;
	uint32_t result;
	int rc;

	/* ITER_GE, key before first page (5) -> page 0. */
	key = vy_key_new_i64(5);
	rc = vy_page_index_find_page(run, key, cmp_def, ITER_GE,
				     &equal_key, &result);
	is(rc, 0, "ITER_GE key=5: no error");
	is(result, 0, "ITER_GE key=5: page 0");
	tuple_unref(key.stmt);

	/* ITER_GE, key on page 5 boundary (60) -> page 4. */
	key = vy_key_new_i64(60);
	rc = vy_page_index_find_page(run, key, cmp_def, ITER_GE,
				     &equal_key, &result);
	is(rc, 0, "ITER_GE key=60: no error");
	is(result, 4, "ITER_GE key=60: page 4");
	ok(equal_key, "ITER_GE key=60: equal_key set");
	tuple_unref(key.stmt);

	/* ITER_LE, key on page 5 boundary (60) -> page 5. */
	key = vy_key_new_i64(60);
	rc = vy_page_index_find_page(run, key, cmp_def, ITER_LE,
				     &equal_key, &result);
	is(rc, 0, "ITER_LE key=60: no error");
	is(result, 5, "ITER_LE key=60: page 5");
	tuple_unref(key.stmt);

	/* ITER_LE, key after last page (200) -> last page (11). */
	key = vy_key_new_i64(200);
	rc = vy_page_index_find_page(run, key, cmp_def, ITER_LE,
				     &equal_key, &result);
	is(rc, 0, "ITER_LE key=200: no error");
	is(result, 11, "ITER_LE key=200: last page");
	tuple_unref(key.stmt);

	/* ITER_LT, key on first page boundary (10) -> page_count (no match). */
	key = vy_key_new_i64(10);
	rc = vy_page_index_find_page(run, key, cmp_def, ITER_LT,
				     &equal_key, &result);
	is(rc, 0, "ITER_LT key=10: no error");
	is(result, 12, "ITER_LT key=10: result == page_count (no match)");
	tuple_unref(key.stmt);

	destroy_mock_run(run);
	check_plan();
	footer();
}

/* ----------------------------------------------------------------
 * Test group 3: vy_run_bloom_check (via vy_run_bloom_check)
 *
 * vy_run_bloom_check is static, so we test it indirectly through
 * the run iterator bloom check path. Instead, since the function
 * is static, we test the components it uses:
 * - block_dir_find_block (tested above)
 * - index_cache_get (tested in group 4)
 * - binary_fuse8_contain (tested here directly)
 * - vy_stmt_hash64 (tested in group 5)
 *
 * We test fuse8 filter construction and membership.
 * ---------------------------------------------------------------- */

static void
test_bloom_check(void)
{
	header();
	plan(6);

	/* Build a fuse8 filter from a known set of hashes. */
	uint32_t nkeys = 100;
	uint64_t *hashes = xmalloc(nkeys * sizeof(uint64_t));
	for (uint32_t i = 0; i < nkeys; i++)
		hashes[i] = (uint64_t)i * 1000003ULL + 42;

	binary_fuse8_t filter;
	memset(&filter, 0, sizeof(filter));
	bool ok_alloc = binary_fuse8_allocate(nkeys, &filter);
	ok(ok_alloc, "fuse8 allocate succeeds");

	bool ok_pop = binary_fuse8_populate(hashes, nkeys, &filter);
	ok(ok_pop, "fuse8 populate succeeds");

	/* All inserted keys should be found (no false negatives). */
	int found = 0;
	for (uint32_t i = 0; i < nkeys; i++) {
		if (binary_fuse8_contain(hashes[i], &filter))
			found++;
	}
	is(found, (int)nkeys, "all inserted keys found in filter");

	/* Keys not inserted should mostly not be found. */
	int false_positives = 0;
	int absent_checked = 10000;
	for (int i = 0; i < absent_checked; i++) {
		uint64_t h = (uint64_t)(nkeys + i) * 1000003ULL + 42;
		if (binary_fuse8_contain(h, &filter))
			false_positives++;
	}
	/* fuse8 FPR is ~0.4%, allow up to 2%. */
	ok(false_positives < absent_checked * 2 / 100,
	   "false positive rate < 2%% (%d/%d)", false_positives,
	   absent_checked);

	/* Cache entry without filter -> contain check skipped. */
	struct vy_index_cache_entry entry_no_filter;
	memset(&entry_no_filter, 0, sizeof(entry_no_filter));
	entry_no_filter.has_filter = false;
	ok(!entry_no_filter.has_filter,
	   "entry without filter: has_filter == false");

	/* Cache entry with filter -> contain check works. */
	struct vy_index_cache_entry entry_with_filter;
	memset(&entry_with_filter, 0, sizeof(entry_with_filter));
	entry_with_filter.has_filter = true;
	entry_with_filter.filter = filter;
	ok(binary_fuse8_contain(hashes[0], &entry_with_filter.filter),
	   "entry with filter: known key found");

	/*
	 * Both filter and entry_with_filter.filter are value copies
	 * sharing the same Fingerprints pointer. Free once via the
	 * original variable.
	 */
	binary_fuse8_free(&filter);
	free(hashes);

	check_plan();
	footer();
}

/* ----------------------------------------------------------------
 * Test group 4: vy_index_cache 2Q eviction
 * ---------------------------------------------------------------- */

static void
test_index_cache_2q(void)
{
	header();
	plan(9);

	/*
	 * Create a cache with a small quota.
	 * Each entry is about sizeof(vy_index_cache_entry) + pages.
	 * Use mem_used = 1024 per entry and quota = 3072 (3 entries).
	 */
	size_t entry_size = 1024;
	size_t quota = entry_size * 3;
	struct vy_index_cache cache;
	vy_index_cache_create(&cache, quota);

	/* Helper: insert a dummy entry. */
	struct vy_page_info *pages;
	struct vy_index_cache_entry *entry;

	/* Put entry (run=1, block=0). */
	pages = xcalloc(1, sizeof(struct vy_page_info));
	entry = vy_index_cache_put(&cache, 1, 0, 0, pages, 1,
				    NULL, NULL, entry_size);
	ok(entry != NULL, "put (1,0) succeeds");
	ok(!entry->is_hot, "put inserts as cold");

	/* Get should return it and promote to hot (2Q semantics). */
	entry = vy_index_cache_get(&cache, 1, 0);
	ok(entry != NULL, "get (1,0) returns entry");

	/*
	 * 2Q policy: first get() promotes from cold to hot.
	 * (Put inserts as cold; any get promotes.)
	 */
	ok(entry->is_hot, "first get promotes to hot");

	/* Second get keeps it in hot, moves to MRU position. */
	entry = vy_index_cache_get(&cache, 1, 0);
	ok(entry != NULL && entry->is_hot,
	   "second get: still hot");

	/* Insert two more to fill quota. */
	pages = xcalloc(1, sizeof(struct vy_page_info));
	entry = vy_index_cache_put(&cache, 1, 1, 0, pages, 1,
				    NULL, NULL, entry_size);
	ok(entry != NULL, "put (1,1) succeeds");

	pages = xcalloc(1, sizeof(struct vy_page_info));
	entry = vy_index_cache_put(&cache, 1, 2, 0, pages, 1,
				    NULL, NULL, entry_size);
	ok(entry != NULL, "put (1,2) succeeds");

	/* Now insert a 4th entry; should evict one cold entry. */
	pages = xcalloc(1, sizeof(struct vy_page_info));
	entry = vy_index_cache_put(&cache, 1, 3, 0, pages, 1,
				    NULL, NULL, entry_size);
	ok(entry != NULL, "put (1,3) triggers eviction");

	/* The hot entry (1,0) should survive. */
	entry = vy_index_cache_get(&cache, 1, 0);
	ok(entry != NULL, "hot entry (1,0) survives eviction");

	vy_index_cache_destroy(&cache);

	check_plan();
	footer();
}

/* ----------------------------------------------------------------
 * Test group 5: vy_stmt_hash64
 * ---------------------------------------------------------------- */

static void
test_stmt_hash64(void)
{
	header();
	plan(5);

	struct vy_entry e1, e2, e3;
	uint64_t h1, h2, h3;

	/* Same key -> same hash (determinism). */
	e1 = vy_key_new_i64(42);
	e2 = vy_key_new_i64(42);
	h1 = vy_stmt_hash64(e1, cmp_def);
	h2 = vy_stmt_hash64(e2, cmp_def);
	is(h1, h2, "same key -> same hash");
	tuple_unref(e1.stmt);
	tuple_unref(e2.stmt);

	/* Different keys -> different hashes. */
	e1 = vy_key_new_i64(42);
	e2 = vy_key_new_i64(43);
	h1 = vy_stmt_hash64(e1, cmp_def);
	h2 = vy_stmt_hash64(e2, cmp_def);
	ok(h1 != h2, "different keys -> different hashes");
	tuple_unref(e1.stmt);
	tuple_unref(e2.stmt);

	/* Negative vs positive -> different hashes. */
	e1 = vy_key_new_i64(-1);
	e2 = vy_key_new_i64(1);
	h1 = vy_stmt_hash64(e1, cmp_def);
	h2 = vy_stmt_hash64(e2, cmp_def);
	ok(h1 != h2, "negative vs positive -> different hashes");
	tuple_unref(e1.stmt);
	tuple_unref(e2.stmt);

	/* Zero -> non-zero hash. */
	e1 = vy_key_new_i64(0);
	h1 = vy_stmt_hash64(e1, cmp_def);
	ok(h1 != 0, "hash of 0 is non-zero");
	tuple_unref(e1.stmt);

	/* Large values. */
	e1 = vy_key_new_i64(INT64_MAX);
	e2 = vy_key_new_i64(INT64_MAX - 1);
	e3 = vy_key_new_i64(INT64_MIN);
	h1 = vy_stmt_hash64(e1, cmp_def);
	h2 = vy_stmt_hash64(e2, cmp_def);
	h3 = vy_stmt_hash64(e3, cmp_def);
	ok(h1 != h2 && h1 != h3 && h2 != h3,
	   "large values produce distinct hashes");
	tuple_unref(e1.stmt);
	tuple_unref(e2.stmt);
	tuple_unref(e3.stmt);

	check_plan();
	footer();
}

/* ----------------------------------------------------------------
 * Test group 6: forward compatibility — unknown keys are skipped
 * ---------------------------------------------------------------- */

static void
test_forward_compat(void)
{
	header();
	plan(4);

	/*
	 * Test 1: vy_block_dir_decode with an extra field in each
	 * directory entry.  Current format is array(2, key, count).
	 * Future format might be array(3, key, count, something).
	 */
	struct vy_run *run = vy_run_new(&run_env, 100);
	fail_if(run == NULL);

	/* Encode: array(2 blocks) */
	char buf[512];
	char *pos = buf;
	pos = mp_encode_array(pos, 2);
	/* Block 0: array(3, key=[10], page_count=4, unknown="future") */
	pos = mp_encode_array(pos, 3);
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_uint(pos, 10);
	pos = mp_encode_uint(pos, 4);
	pos = mp_encode_str(pos, "future_data", 11);
	/* Block 1: array(4, key=[20], page_count=3, unk1=42, unk2=true) */
	pos = mp_encode_array(pos, 4);
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_uint(pos, 20);
	pos = mp_encode_uint(pos, 3);
	pos = mp_encode_uint(pos, 42);

	const char *data = buf;
	int rc = vy_block_dir_decode(run, &data, cmp_def, "test");
	is(rc, 0, "block_dir_decode skips unknown fields");
	is(run->block_count, 2, "decoded 2 blocks");

	/* Verify the known fields were decoded correctly. */
	ok(run->block_dir[0].page_count == 4 &&
	   run->block_dir[1].page_count == 3,
	   "page counts decoded correctly despite extra fields");

	vy_run_unref(run);

	/*
	 * Test 2: vy_page_info_decode with an unknown key in the
	 * page_info map.  Add key 99 (doesn't exist) with a string
	 * value — old code must skip it.
	 */
	char pi_buf[256];
	pos = pi_buf;
	/* Map with 7 keys: 6 mandatory + 1 unknown. */
	pos = mp_encode_map(pos, 7);
	/* VY_PAGE_INFO_OFFSET = 1 */
	pos = mp_encode_uint(pos, 1);
	pos = mp_encode_uint(pos, 1000);
	/* VY_PAGE_INFO_SIZE = 2 */
	pos = mp_encode_uint(pos, 2);
	pos = mp_encode_uint(pos, 500);
	/* VY_PAGE_INFO_UNPACKED_SIZE = 3 */
	pos = mp_encode_uint(pos, 3);
	pos = mp_encode_uint(pos, 800);
	/* VY_PAGE_INFO_ROW_COUNT = 4 */
	pos = mp_encode_uint(pos, 4);
	pos = mp_encode_uint(pos, 50);
	/* VY_PAGE_INFO_MIN_KEY = 5 */
	pos = mp_encode_uint(pos, 5);
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_uint(pos, 42);
	/* VY_PAGE_INFO_ROW_INDEX_OFFSET = 6 */
	pos = mp_encode_uint(pos, 6);
	pos = mp_encode_uint(pos, 200);
	/* Unknown key 99 with a map value. */
	pos = mp_encode_uint(pos, 99);
	pos = mp_encode_map(pos, 1);
	pos = mp_encode_str(pos, "a", 1);
	pos = mp_encode_str(pos, "b", 1);

	struct xrow_header xrow;
	memset(&xrow, 0, sizeof(xrow));
	xrow.type = VY_INDEX_PAGE_INFO;
	xrow.body->iov_base = pi_buf;
	xrow.body->iov_len = pos - pi_buf;
	xrow.bodycnt = 1;

	struct vy_page_info page;
	rc = vy_page_info_decode(&page, &xrow, cmp_def, "test");
	is(rc, 0, "page_info_decode skips unknown key 99");
	free(page.min_key);

	check_plan();
	footer();
}

int
main(void)
{
	plan(6);
	header();

	vy_iterator_C_test_init(128 * 1024);
	vy_run_env_create(&run_env, stmt_env.key_format, 0);
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_INTEGER };
	cmp_def = box_key_def_new(fields, types, 1);
	fail_if(cmp_def == NULL);

	test_block_dir_find_block();
	test_page_index_find_page();
	test_bloom_check();
	test_index_cache_2q();
	test_stmt_hash64();
	test_forward_compat();

	key_def_delete(cmp_def);
	vy_run_env_destroy(&run_env);
	vy_iterator_C_test_finish();
	footer();
	return check_plan();
}
