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
#include "vy_index_cache.h"

#include <assert.h>
#include <stdlib.h>

#include "diag.h"
#include "vy_run.h" /* for struct vy_page_info */
#include "trivia/util.h"
#include "PMurHash.h"

/*
 * The fraction of mem_quota allocated to the cold (A1) queue.
 * Entries promoted to hot take from the remaining 75%.
 */
enum { VY_INDEX_CACHE_COLD_PCT = 25 };

/**
 * Extent size for the light hash table.  16KB is the same as
 * MEMTX_EXTENT_SIZE and gives a good balance between allocation
 * granularity and internal fragmentation.
 */
enum { VY_INDEX_CACHE_EXTENT_SIZE = 16 * 1024 };

/* -------------------- hash table (light.h) -------------------- */

/**
 * Hash function for (run_id, block_no) using PMurHash32.
 *
 * We hash the raw bytes of run_id and block_no individually
 * to avoid struct padding issues (the struct would have 4 bytes
 * of padding after block_no that could be uninitialized).
 */
static inline uint32_t
vy_idx_cache_hash(int64_t run_id, uint32_t block_no)
{
	uint32_t h1 = 0;
	uint32_t carry = 0;
	PMurHash32_Process(&h1, &carry, &run_id, sizeof(run_id));
	PMurHash32_Process(&h1, &carry, &block_no, sizeof(block_no));
	return PMurHash32_Result(h1, carry,
				 sizeof(run_id) + sizeof(block_no));
}

static inline bool
vy_idx_cache_equal(struct vy_index_cache_entry *a,
		   struct vy_index_cache_entry *b, int unused)
{
	(void)unused;
	return a->run_id == b->run_id && a->block_no == b->block_no;
}

static inline bool
vy_idx_cache_equal_key(struct vy_index_cache_entry *entry,
		       struct vy_idx_cache_key *key, int unused)
{
	(void)unused;
	return entry->run_id == key->run_id &&
	       entry->block_no == key->block_no;
}

#define LIGHT_NAME _idx_cache
#define LIGHT_DATA_TYPE struct vy_index_cache_entry *
#define LIGHT_KEY_TYPE struct vy_idx_cache_key *
#define LIGHT_CMP_ARG_TYPE int
#define LIGHT_EQUAL(a, b, c) vy_idx_cache_equal(a, b, c)
#define LIGHT_EQUAL_KEY(a, b, c) vy_idx_cache_equal_key(a, b, c)

#include "salad/light.h"

#undef LIGHT_NAME
#undef LIGHT_DATA_TYPE
#undef LIGHT_KEY_TYPE
#undef LIGHT_CMP_ARG_TYPE
#undef LIGHT_EQUAL
#undef LIGHT_EQUAL_KEY

/* -------------------- extent allocator -------------------- */

static void *
vy_index_cache_extent_alloc(void *ctx)
{
	struct vy_index_cache *cache = (struct vy_index_cache *)ctx;
	return mempool_alloc(&cache->extent_pool);
}

static void
vy_index_cache_extent_free(void *ctx, void *extent)
{
	struct vy_index_cache *cache = (struct vy_index_cache *)ctx;
	mempool_free(&cache->extent_pool, extent);
}

/* -------------------- helpers -------------------- */

/**
 * Destroy a single page_info entry (free its min_key).
 */
static void
vy_index_cache_page_info_destroy(struct vy_page_info *pi)
{
	if (pi->min_key != NULL)
		free(pi->min_key);
}

/**
 * Free a cache entry: destroy page_info entries, free the pages
 * array, and release the entry back to the mempool.
 */
static void
vy_index_cache_entry_delete(struct vy_index_cache *cache,
			    struct vy_index_cache_entry *entry)
{
	for (uint32_t i = 0; i < entry->page_count; i++)
		vy_index_cache_page_info_destroy(&entry->pages[i]);
	free(entry->pages);
	if (entry->has_filter)
		binary_fuse8_free(&entry->filter);
	mempool_free(&cache->entry_pool, entry);
}

/**
 * Remove an entry from its queue and the hash table, update
 * accounting, and free it.
 */
static void
vy_index_cache_evict(struct vy_index_cache *cache,
		     struct vy_index_cache_entry *entry)
{
	/* Remove from queue. */
	rlist_del_entry(entry, in_queue);
	if (entry->is_hot)
		cache->hot_mem -= entry->mem_used;
	else
		cache->cold_mem -= entry->mem_used;

	/* Remove from hash table. */
	uint32_t h = vy_idx_cache_hash(entry->run_id, entry->block_no);
	struct vy_idx_cache_key key = {
		.run_id = entry->run_id,
		.block_no = entry->block_no,
	};
	struct light_idx_cache_core *ht =
		(struct light_idx_cache_core *)cache->ht;
	uint32_t pos = light_idx_cache_find_key(ht, h, &key);
	assert(pos != light_idx_cache_end);
	light_idx_cache_delete(ht, pos);

	vy_index_cache_entry_delete(cache, entry);
	cache->stat.evict++;
}

/**
 * Evict entries until total memory usage is at or below quota.
 * Prefers evicting cold entries (FIFO), then hot entries (LRU).
 */
static void
vy_index_cache_gc(struct vy_index_cache *cache)
{
	size_t total = cache->cold_mem + cache->hot_mem;
	while (total > cache->mem_quota) {
		struct vy_index_cache_entry *victim;
		if (!rlist_empty(&cache->cold)) {
			victim = rlist_last_entry(&cache->cold,
					struct vy_index_cache_entry,
					in_queue);
		} else if (!rlist_empty(&cache->hot)) {
			victim = rlist_last_entry(&cache->hot,
					struct vy_index_cache_entry,
					in_queue);
		} else {
			break;
		}
		size_t freed = victim->mem_used;
		vy_index_cache_evict(cache, victim);
		total -= freed;
	}
}

/**
 * If the cold queue exceeds its limit, evict the oldest cold
 * entry.  This keeps the cold queue at ~25% of quota.
 */
static void
vy_index_cache_gc_cold(struct vy_index_cache *cache)
{
	while (cache->cold_mem > cache->cold_limit &&
	       !rlist_empty(&cache->cold)) {
		struct vy_index_cache_entry *victim;
		victim = rlist_last_entry(&cache->cold,
				struct vy_index_cache_entry,
				in_queue);
		vy_index_cache_evict(cache, victim);
	}
}

/* -------------------- public API -------------------- */

void
vy_index_cache_create(struct vy_index_cache *cache, size_t mem_quota)
{
	rlist_create(&cache->cold);
	rlist_create(&cache->hot);
	cache->cold_mem = 0;
	cache->hot_mem = 0;
	cache->mem_quota = mem_quota;
	cache->cold_limit = mem_quota * VY_INDEX_CACHE_COLD_PCT / 100;

	/*
	 * Set the arena quota to QUOTA_MAX: the actual memory limit
	 * is enforced at the application level by the 2Q eviction
	 * logic (using cache->mem_quota), not by the slab arena.
	 *
	 * The arena/slab_cache/mempool chain still provides
	 * efficient block-based allocation.  We can't use a tight
	 * arena quota because slab caches may hold freed slabs
	 * internally, making quota_use fail even after eviction.
	 */
	quota_init(&cache->quota, QUOTA_MAX);

	slab_arena_create(&cache->arena, &cache->quota, 0,
			  4 * 1024 * 1024, SLAB_ARENA_PRIVATE);
	slab_cache_create(&cache->slab_cache, &cache->arena);

	mempool_create(&cache->entry_pool, &cache->slab_cache,
		       sizeof(struct vy_index_cache_entry));
	mempool_create(&cache->extent_pool, &cache->slab_cache,
		       VY_INDEX_CACHE_EXTENT_SIZE);

	struct light_idx_cache_core *ht =
		(struct light_idx_cache_core *)
		calloc(1, sizeof(struct light_idx_cache_core));
	if (ht == NULL)
		panic("failed to allocate vy_index_cache hash table");
	light_idx_cache_create(ht, VY_INDEX_CACHE_EXTENT_SIZE,
			       vy_index_cache_extent_alloc,
			       vy_index_cache_extent_free,
			       cache, 0);
	cache->ht = ht;

	rlist_create(&cache->loading);

	cache->stat.hit = 0;
	cache->stat.miss = 0;
	cache->stat.evict = 0;
}

void
vy_index_cache_destroy(struct vy_index_cache *cache)
{
	struct vy_index_cache_entry *entry, *tmp;
	rlist_foreach_entry_safe(entry, &cache->cold, in_queue, tmp)
		vy_index_cache_entry_delete(cache, entry);
	rlist_foreach_entry_safe(entry, &cache->hot, in_queue, tmp)
		vy_index_cache_entry_delete(cache, entry);

	struct light_idx_cache_core *ht =
		(struct light_idx_cache_core *)cache->ht;
	light_idx_cache_destroy(ht);
	free(ht);

	mempool_destroy(&cache->extent_pool);
	mempool_destroy(&cache->entry_pool);
	slab_cache_destroy(&cache->slab_cache);
	slab_arena_destroy(&cache->arena);
}

void
vy_index_cache_set_quota(struct vy_index_cache *cache, size_t mem_quota)
{
	cache->mem_quota = mem_quota;
	cache->cold_limit = mem_quota * VY_INDEX_CACHE_COLD_PCT / 100;
	vy_index_cache_gc(cache);
}

struct vy_index_cache_entry *
vy_index_cache_get(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no)
{
	struct light_idx_cache_core *ht =
		(struct light_idx_cache_core *)cache->ht;
	struct vy_idx_cache_key key = {
		.run_id = run_id,
		.block_no = block_no,
	};
	uint32_t h = vy_idx_cache_hash(run_id, block_no);
	uint32_t pos = light_idx_cache_find_key(ht, h, &key);
	if (pos == light_idx_cache_end)
		return NULL;

	struct vy_index_cache_entry *entry = light_idx_cache_get(ht, pos);

	if (!entry->is_hot) {
		/*
		 * Promote from cold to hot.  This is the key
		 * property of 2Q: a second access proves the entry
		 * is "useful" and should be protected from scans.
		 */
		rlist_del_entry(entry, in_queue);
		cache->cold_mem -= entry->mem_used;

		rlist_add_entry(&cache->hot, entry, in_queue);
		cache->hot_mem += entry->mem_used;
		entry->is_hot = true;
	} else {
		/* Move to the head of the hot (LRU) queue. */
		rlist_del_entry(entry, in_queue);
		rlist_add_entry(&cache->hot, entry, in_queue);
	}
	return entry;
}

struct vy_index_cache_entry *
vy_index_cache_put(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no,
		   uint32_t first_page_no,
		   struct vy_page_info *pages, uint32_t page_count,
		   binary_fuse8_t *filter, struct minhash *sketch,
		   size_t mem_used)
{
	/*
	 * Enforce cold queue limit first, then enforce total
	 * memory limit.  Both are done before allocation so that
	 * the freshly inserted entry is guaranteed to survive.
	 */
	vy_index_cache_gc_cold(cache);
	size_t total = cache->cold_mem + cache->hot_mem + mem_used;
	if (total > cache->mem_quota) {
		size_t saved = cache->mem_quota;
		cache->mem_quota = saved >= mem_used ?
				   saved - mem_used : 0;
		vy_index_cache_gc(cache);
		cache->mem_quota = saved;
	}

	struct vy_index_cache_entry *entry =
		(struct vy_index_cache_entry *)
		mempool_alloc(&cache->entry_pool);
	if (entry == NULL) {
		diag_set(OutOfMemory, sizeof(*entry),
			 "mempool", "vy_index_cache_entry");
		return NULL;
	}

	entry->run_id = run_id;
	entry->block_no = block_no;
	entry->first_page_no = first_page_no;
	entry->page_count = page_count;
	entry->pages = pages;
	entry->mem_used = mem_used;
	entry->is_hot = false;

	/* Transfer ownership of filter if present. */
	if (filter != NULL) {
		entry->filter = *filter;
		entry->has_filter = true;
	} else {
		memset(&entry->filter, 0, sizeof(entry->filter));
		entry->has_filter = false;
	}

	/* Copy sketch if present. */
	if (sketch != NULL) {
		entry->sketch = *sketch;
		entry->has_sketch = true;
	} else {
		minhash_create(&entry->sketch);
		entry->has_sketch = false;
	}

	/* Insert into cold queue (FIFO: newest at head). */
	rlist_add_entry(&cache->cold, entry, in_queue);
	cache->cold_mem += mem_used;

	/* Insert into hash table. */
	struct light_idx_cache_core *ht =
		(struct light_idx_cache_core *)cache->ht;
	uint32_t h = vy_idx_cache_hash(run_id, block_no);
	uint32_t pos = light_idx_cache_insert(ht, h, entry);
	if (pos == light_idx_cache_end) {
		rlist_del_entry(entry, in_queue);
		cache->cold_mem -= mem_used;
		/*
		 * Don't free pages or filter here: ownership
		 * has not been transferred on failure, so the
		 * caller is responsible for cleanup.
		 */
		mempool_free(&cache->entry_pool, entry);
		diag_set(OutOfMemory, 0, "light_idx_cache",
			 "vy_index_cache_put");
		return NULL;
	}

	/*
	 * Enforce limits.  Note: we do NOT evict any entries
	 * here to guarantee that the just-inserted entry
	 * survives and is immediately accessible to the caller.
	 * Eviction was already performed above before allocation.
	 */
	return entry;
}
