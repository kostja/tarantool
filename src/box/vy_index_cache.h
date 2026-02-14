#ifndef INCLUDES_TARANTOOL_BOX_VY_INDEX_CACHE_H
#define INCLUDES_TARANTOOL_BOX_VY_INDEX_CACHE_H
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

/**
 * @file
 *
 * 2Q cache for Vinyl .index2 blocks.
 *
 * Each cache entry stores a decoded array of vy_page_info for one
 * index block (up to VY_INDEX_BLOCK_SIZE pages), keyed by
 * (run_id, block_no).
 *
 * The 2Q eviction policy is scan-resistant:
 *  - First-time accesses go into the "cold" (A1in) FIFO queue.
 *  - When a cold entry is accessed again, it is promoted to the
 *    "hot" (Am) LRU queue.
 *  - Eviction prefers cold entries (FIFO order); if the cold queue
 *    is empty, the least-recently-used hot entry is evicted.
 *  - The cold queue is capped at 25% of the total memory quota.
 *
 * Memory management uses a dedicated quota → slab_arena →
 * slab_cache → mempool chain from src/lib/small, so the total
 * memory used by the cache is bounded by the box.cfg
 * vinyl_index_cache setting.
 *
 * The hash table is the block-based light hash from
 * src/lib/salad/light.h, backed by the same slab_cache.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <small/rlist.h>
#include <small/mempool.h>
#include <small/quota.h>
#include <small/slab_arena.h>
#include <small/slab_cache.h>

#include "salad/minhash.h"
#include "binaryfusefilter.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_page_info;

/**
 * A single cached index block.
 */
struct vy_index_cache_entry {
	/** Link in the cold (A1) or hot (Am) queue. */
	struct rlist in_queue;
	/** True if in hot queue (Am), false if in cold queue (A1). */
	bool is_hot;
	/** Run ID — part of cache key. */
	int64_t run_id;
	/** Index block number — part of cache key. */
	uint32_t block_no;
	/** Number of page_info entries in this block. */
	uint32_t page_count;
	/** Global page number of the first page in this block. */
	uint32_t first_page_no;
	/**
	 * Array of decoded page_info entries.
	 * Allocated separately; freed when the entry is evicted.
	 */
	struct vy_page_info *pages;
	/**
	 * Binary fuse8 membership filter for this block.
	 * If has_filter is false, no filter is available
	 * (e.g. runs upgraded from v1 format).
	 */
	binary_fuse8_t filter;
	bool has_filter;
	/**
	 * MinHash overlap sketch for this block.
	 * If has_sketch is false, no sketch is available.
	 */
	struct minhash sketch;
	bool has_sketch;
	/** Memory occupied by this entry (struct + pages + keys + filter). */
	size_t mem_used;
};

/**
 * Lookup key for the hash table: (run_id, block_no).
 */
struct vy_idx_cache_key {
	int64_t run_id;
	uint32_t block_no;
};

/* Forward-declare the light hash types (instantiated in .c). */
struct light_idx_cache_core;
struct light_idx_cache_iterator;

/**
 * 2Q index cache.
 */
struct vy_index_cache {
	/** Cold queue (A1in) — FIFO, for first-time accesses. */
	struct rlist cold;
	/** Hot queue (Am) — LRU, for repeated accesses. */
	struct rlist hot;
	/** Memory used by cold queue entries. */
	size_t cold_mem;
	/** Memory used by hot queue entries. */
	size_t hot_mem;
	/** Total memory limit. */
	size_t mem_quota;
	/**
	 * Cold queue size limit.
	 * Set to 25% of mem_quota.
	 */
	size_t cold_limit;
	/** Memory quota enforced by small allocators. */
	struct quota quota;
	/** Slab arena backed by the quota. */
	struct slab_arena arena;
	/** Slab cache backed by the arena. */
	struct slab_cache slab_cache;
	/** Mempool for vy_index_cache_entry structs. */
	struct mempool entry_pool;
	/** Mempool for light hash extents. */
	struct mempool extent_pool;
	/**
	 * Light hash table for O(1) lookup by (run_id, block_no).
	 * Allocated on the heap since the type is generated by the
	 * macro and only visible in the .c file.
	 */
	void *ht;
	/**
	 * List of in-flight block load sentinels.
	 * Used to prevent duplicate disk reads when multiple fibers
	 * concurrently miss the same cache entry.  Each entry is a
	 * stack-allocated struct owned by the loading fiber.
	 */
	struct rlist loading;
	/** Statistics. */
	struct {
		/** Number of cache hits (block found). */
		int64_t hit;
		/** Number of cache misses (block loaded from disk). */
		int64_t miss;
		/** Number of blocks evicted. */
		int64_t evict;
	} stat;
};

/**
 * Initialize the index cache.
 *
 * @param cache     Cache to initialize.
 * @param mem_quota Memory limit in bytes (0 = unlimited).
 */
void
vy_index_cache_create(struct vy_index_cache *cache, size_t mem_quota);

/**
 * Destroy the index cache, freeing all entries.
 */
void
vy_index_cache_destroy(struct vy_index_cache *cache);

/**
 * Set a new memory quota.
 *
 * If the new quota is smaller than current usage, excess
 * entries are evicted immediately via vy_index_cache_gc().
 */
void
vy_index_cache_set_quota(struct vy_index_cache *cache, size_t mem_quota);

/**
 * Look up an index block in the cache.
 *
 * If found, the entry is "touched" (promoted to hot if cold,
 * or moved to the head of hot if already hot).
 *
 * @param cache    The cache.
 * @param run_id   Run identifier.
 * @param block_no Index block number within the run.
 * @return Cache entry, or NULL if not found.
 */
struct vy_index_cache_entry *
vy_index_cache_get(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no);

/**
 * Insert an index block into the cache.
 *
 * The entry goes into the cold (A1) queue.  If the cache is over
 * quota, older entries are evicted first.
 *
 * Ownership of @a pages is transferred to the cache: the cache
 * will free the array (and the min_key strings inside each
 * vy_page_info) when the entry is evicted.
 *
 * If @a filter is non-NULL, ownership of filter->Fingerprints
 * is transferred to the cache.  If @a sketch is non-NULL, it
 * is copied into the cache entry.
 *
 * @param cache         The cache.
 * @param run_id        Run identifier.
 * @param block_no      Index block number within the run.
 * @param first_page_no Global page number of the first page.
 * @param pages         Array of decoded vy_page_info entries.
 * @param page_count    Number of entries in @a pages.
 * @param filter        Binary fuse8 filter, or NULL if unavailable.
 * @param sketch        MinHash sketch, or NULL if unavailable.
 * @param mem_used      Total memory consumed (struct + pages + keys + filter).
 * @return Inserted entry on success, NULL on memory error.
 *         The entry is in the cold queue (not promoted to hot).
 */
struct vy_index_cache_entry *
vy_index_cache_put(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no,
		   uint32_t first_page_no,
		   struct vy_page_info *pages, uint32_t page_count,
		   binary_fuse8_t *filter, struct minhash *sketch,
		   size_t mem_used);

/** Return total memory used by the cache. */
static inline size_t
vy_index_cache_mem_used(const struct vy_index_cache *cache)
{
	return cache->cold_mem + cache->hot_mem;
}

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_INDEX_CACHE_H */
