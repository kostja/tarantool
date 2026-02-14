/*
 * Stub implementation of vy_index_cache for unit tests that
 * link vy_run.c but don't exercise v2 page index lookups.
 */
#include "vy_index_cache.h"

void
vy_index_cache_create(struct vy_index_cache *cache, size_t mem_quota)
{
	(void)cache;
	(void)mem_quota;
}

void
vy_index_cache_destroy(struct vy_index_cache *cache)
{
	(void)cache;
}

void
vy_index_cache_set_quota(struct vy_index_cache *cache, size_t mem_quota)
{
	(void)cache;
	(void)mem_quota;
}

struct vy_index_cache_entry *
vy_index_cache_get(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no)
{
	(void)cache;
	(void)run_id;
	(void)block_no;
	return NULL;
}

struct vy_index_cache_entry *
vy_index_cache_put(struct vy_index_cache *cache,
		   int64_t run_id, uint32_t block_no,
		   uint32_t first_page_no,
		   struct vy_page_info *pages, uint32_t page_count,
		   binary_fuse8_t *filter, struct minhash *sketch,
		   size_t mem_used)
{
	(void)cache;
	(void)run_id;
	(void)block_no;
	(void)first_page_no;
	(void)pages;
	(void)page_count;
	(void)filter;
	(void)sketch;
	(void)mem_used;
	return NULL;
}
