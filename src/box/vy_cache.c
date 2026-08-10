/*
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
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
#include "vy_cache.h"
#include "bit/bit.h"
#include "diag.h"
#include "fiber.h"
#include "schema_def.h"
#include "vy_history.h"
#include "vy_read_view.h"
#include "vy_tx.h"

#ifndef CT_ASSERT_G
#define CT_ASSERT_G(e) typedef char CONCAT(__ct_assert_, __LINE__)[(e) ? 1 :-1]
#endif

CT_ASSERT_G(BOX_INDEX_PART_MAX <= UINT8_MAX);
CT_ASSERT_G(VY_CACHE_HEAT_MAX <=
	    VY_STMT_HEAT_MASK >> VY_STMT_HEAT_SHIFT);

enum {
	/*
	 * The eviction walk budget, in walked bytes per admitted
	 * byte. An untouched entry is evicted in two visits --
	 * the admission credit grants it one cycle -- a
	 * saturated one in as many visits as its heat counts.
	 * Sustained admission pressure of (heat + 1) / factor
	 * times the resident bytes therefore cycles out even the
	 * hottest entry; until then, admissions that do not fit
	 * are refused. The factor trades admission rate under
	 * pressure for walk cost, not correctness.
	 */
	VY_CACHE_GC_FACTOR = 2,
	/*
	 * The admission credit: a fresh entry enters with enough
	 * heat to survive one hand visit, so it gets one full
	 * cycle of reuse window wherever it lands relative to
	 * the hand. Anything below leaves the window to a
	 * positional lottery: an entry admitted just ahead of
	 * the hand would be evicted before any chance of a
	 * second read. The credit also keeps the eviction walk
	 * live, see vy_cache_env_gc(): it must stay >= 2, or an
	 * insertion stream could feed the hand an eviction at
	 * every step and pin it in one cache forever.
	 */
	VY_CACHE_ADMIT_HEAT = 2,
};

/** An entry's accounted size, see the vy_cache section. */
static size_t
vy_cache_entry_size(bool is_primary, const struct vy_cache_entry *node);

/** A chain metadata statement, see the vy_cache section. */
static bool
vy_stmt_is_cache_meta(struct tuple *stmt);

/** Clear a statement's PK_CACHED bit, see the vy_cache section. */
static void
vy_stmt_clear_pk_cached(struct tuple *stmt, bool is_primary);

/**
 * Track a resident primary tuple: the mean resident tuple size
 * is the eviction hand's weighting unit, see vy_cache_env_gc().
 * Metadata and secondary entries do not shift the mean.
 */
static void
vy_cache_env_acct_tuple(struct vy_cache_env *env, struct tuple *stmt,
			bool is_primary)
{
	if (is_primary && !vy_stmt_is_cache_meta(stmt))
		vy_stmt_counter_acct_tuple(&env->tuple, stmt);
}

/** Mirror of vy_cache_env_acct_tuple. */
static void
vy_cache_env_unacct_tuple(struct vy_cache_env *env, struct tuple *stmt,
			  bool is_primary)
{
	if (is_primary && !vy_stmt_is_cache_meta(stmt))
		vy_stmt_counter_unacct_tuple(&env->tuple, stmt);
}

/* {{{ vy_cache_drain */

/** A dropped cache: a detached tree pending release. */
struct vy_cache_drain_entry {
	/** A member of vy_cache_drain::queue. */
	struct rlist in_queue;
	/** The detached tree; entries up to @a pos are released. */
	struct vy_cache_tree *tree;
	/** The release walk's position. */
	struct vy_cache_tree_iterator pos;
	/** The dropped cache was a primary index's (accounting). */
	bool is_primary;
};

static void
vy_cache_drain_create(struct vy_cache_drain *drain)
{
	rlist_create(&drain->queue);
}

/** Stow the detached tree of a dropped cache for release. */
static void
vy_cache_drain_add(struct vy_cache_env *env, struct vy_cache_tree *tree,
		   bool is_primary)
{
	struct vy_cache_drain_entry *drained = xmalloc(sizeof(*drained));
	drained->tree = tree;
	drained->pos = vy_cache_tree_first(tree);
	drained->is_primary = is_primary;
	rlist_add_tail_entry(&env->drain.queue, drained, in_queue);
}

/**
 * Release up to @a budget accounted bytes of the dropped caches,
 * oldest first. The walk needs no comparator and deletes
 * nothing: entries are only unreferenced, so a position stays
 * valid across calls, and a tree is destroyed wholesale once its
 * walk is over.
 * @return the accounted bytes released.
 */
static size_t
vy_cache_drain_step(struct vy_cache_env *env, size_t budget)
{
	struct vy_cache_drain *drain = &env->drain;
	size_t released = 0;
	while (released < budget && !rlist_empty(&drain->queue)) {
		struct vy_cache_drain_entry *drained =
			rlist_first_entry(&drain->queue,
					  struct vy_cache_drain_entry,
					  in_queue);
		while (released < budget &&
		       !vy_cache_tree_iterator_is_invalid(&drained->pos)) {
			struct vy_cache_entry *node =
				vy_cache_tree_iterator_get_elem(
					drained->tree, &drained->pos);
			released += vy_cache_entry_size(drained->is_primary,
							node);
			vy_cache_env_unacct_tuple(env, node->entry.stmt,
						  drained->is_primary);
			vy_stmt_clear_pk_cached(node->entry.stmt,
						drained->is_primary);
			tuple_unref(node->entry.stmt);
			vy_cache_tree_iterator_next(drained->tree,
						    &drained->pos);
		}
		if (!vy_cache_tree_iterator_is_invalid(&drained->pos))
			break;
		vy_cache_tree_destroy(drained->tree);
		free(drained->tree);
		rlist_del(&drained->in_queue);
		free(drained);
	}
	assert(env->mem_used >= released);
	env->mem_used -= released;
	return released;
}

/* }}} vy_cache_drain */

/** The cache's entry eviction, see the vy_cache section. */
static void
vy_cache_evict_pos(struct vy_cache *cache,
		   struct vy_cache_tree_iterator itr);

/**
 * Destroy the pending links facing the position at @a itr, see
 * the vy_cache section.
 */
static void
vy_cache_clear_pending_links(struct vy_cache *cache,
			     struct vy_cache_tree_iterator itr,
			     bool skip_curr);

/* {{{ vy_cache_env */

/** The eviction hand, defined below. */
static void
vy_cache_env_gc(struct vy_cache_env *env, size_t size,
		bool armed);

/**
 * Allocate a cache tree page.
 * @return the new page; allocation failure panics.
 */
static void *
vy_cache_tree_page_alloc(struct matras_allocator *allocator)
{
	(void)allocator;
	return xmalloc(VY_CACHE_TREE_EXTENT_SIZE);
}

static void
vy_cache_tree_page_free(struct matras_allocator *allocator, void *ptr)
{
	(void)allocator;
	free(ptr);
}

int
vy_cache_env_create(struct vy_cache_env *e, struct tuple_format *key_format)
{
	/*
	 * The dup source for the key space end bounds ([]- and
	 * []+). The bounds are inserted as copies, never as
	 * references to a shared key: once inserted into a
	 * cache, a bound is mutated by fusion, which raises its
	 * LSN in place when a DELETE fuses into it.
	 */
	e->empty_key.hint = HINT_NONE;
	e->empty_key.stmt = vy_key_new(key_format, NULL, 0);
	if (e->empty_key.stmt == NULL)
		return -1;
	rlist_create(&e->gc_list);
	vy_cache_drain_create(&e->drain);
	e->gc_cache = NULL;
	e->scan_id = 0;
	e->xm = NULL;
	e->mem_used = 0;
	e->mem_quota = 0;
	e->tuple.rows = 0;
	e->tuple.bytes = 0;
	e->shadow[0] = NULL;
	e->shadow[1] = NULL;
	e->shadow_mask = 0;
	e->shadow_epoch = 0;
	e->shadow_set_bits = 0;
	e->door = NULL;
	e->door_keys = 0;
	e->door_set_bits = 0;
	e->armed_walked = 0;
	matras_allocator_create(&e->allocator,
				VY_CACHE_TREE_EXTENT_SIZE,
				vy_cache_tree_page_alloc,
				vy_cache_tree_page_free);
	return 0;
}

void
vy_cache_env_destroy(struct vy_cache_env *e)
{
#if ENABLE_ASAN
	/*
	 * Purge the caches, dropped ones included, to suppress
	 * the leak detector. A normal shutdown skips the walk:
	 * the process is exiting, freeing must not delay it.
	 */
	e->mem_quota = 0;
	while (e->mem_used > 0)
		vy_cache_env_gc(e, 0, 0);
#endif
	free(e->shadow[0]);
	free(e->shadow[1]);
	free(e->door);
	tuple_unref(e->empty_key.stmt);
	matras_allocator_destroy(&e->allocator);
}

/**
 * @return a fresh scan id; never 0, the no-id sentinel.
 */
static uint64_t
vy_cache_env_new_scan_id(struct vy_cache_env *env)
{
	env->scan_id = (env->scan_id + 1) & VY_CACHE_SCAN_ID_MAX;
	if (env->scan_id == 0)
		env->scan_id = 1;
	return env->scan_id;
}

/* {{{ shadow map */

enum {
	/** Bytes per map array: quota >> SHIFT, clamped. */
	VY_CACHE_SHADOW_QUOTA_SHIFT = 7,
	VY_CACHE_SHADOW_MIN_BYTES = 16 * 1024,
	VY_CACHE_SHADOW_MAX_BYTES = 256 * 1024,
};

/*
 * The shadow map is the cache's memory of key re-reference,
 * kept in three bloom filters: the doorkeeper and the two
 * protected epochs, the young and the old.
 *
 * Every read recorded in the cache reports its key, see
 * vy_cache_insert(). A key seen for the first time is stamped
 * into the doorkeeper and that is all; when the doorkeeper
 * fills to a set density it is wiped. A key found in the
 * doorkeeper -- read twice within the doorkeeper's window --
 * is promoted: stamped into the young protected epoch, as is,
 * on every read, a key already protected. Protection is
 * re-reference the incumbents' heat can no longer outrank,
 * since heat cannot tell a resident the readers still use
 * from a resident of a dead working set that merely has not
 * cooled yet.
 *
 * Protection acts twice in the eviction walk, see
 * vy_cache_env_gc(). A protected key's read runs the walk
 * armed: the eviction threshold rises from zero to the
 * admission credit, and any unprotected entry the credit
 * covers yields its room, cooled or not. And on every walk,
 * armed or not, an entry whose key is in the young epoch is
 * reprieved at any heat. The verdict is recency where heat is
 * blind: a dead set stops being read, drops out of the young
 * epoch, and is evicted by the armed stream of the set
 * replacing it -- a working set change converges about as
 * fast as under plain LRU -- while the live set is restamped
 * on every read and cannot be washed out.
 *
 * The doorkeeper is what makes this scan resistant. A sweep
 * touches each key once, so a sweep never promotes: it fills
 * and refills the doorkeeper, whose window is the resident
 * row count -- a sweep does not fit the cache by definition,
 * so it reappears, if it reappears at all, after its stamps
 * were wiped. Sweep reads run unarmed, never flip the
 * protected epochs, and meet reprieved incumbents: a sweep of
 * any length, one-shot or repeated, is refused admission and
 * cannot displace the protected set. A recurring set short
 * enough to promote is not a sweep -- it fits the cache, and
 * arming lets it claim the room from colder incumbents.
 *
 * The protected epochs age by armed traffic only: the young
 * epoch flips -- the old array is wiped and becomes young --
 * when it fills to a set density, or when armed walks have
 * covered a full revolution of the resident bytes. Either
 * way the flip is driven by promoted keys competing for the
 * cache, never by sweeps. A false positive reprieves or
 * elevates one key -- a nudged eviction order, never a wrong
 * result.
 */

/** Size the arrays for the quota; a change wipes the map. */
static void
vy_cache_env_shadow_resize(struct vy_cache_env *env)
{
	size_t bytes = env->mem_quota >> VY_CACHE_SHADOW_QUOTA_SHIFT;
	if (bytes < VY_CACHE_SHADOW_MIN_BYTES)
		bytes = VY_CACHE_SHADOW_MIN_BYTES;
	if (bytes > VY_CACHE_SHADOW_MAX_BYTES)
		bytes = VY_CACHE_SHADOW_MAX_BYTES;
	if (env->mem_quota == 0)
		bytes = 0;
	/* A power of two, for mask indexing. */
	if (bytes != 0)
		bytes = 1UL << (64 - bit_clz_u64(bytes - 1));
	uint64_t mask = bytes == 0 ? 0 : bytes * 8 - 1;
	if (mask == env->shadow_mask)
		return;
	free(env->shadow[0]);
	free(env->shadow[1]);
	free(env->door);
	env->shadow[0] = bytes != 0 ? xcalloc(bytes / 8, 8) : NULL;
	env->shadow[1] = bytes != 0 ? xcalloc(bytes / 8, 8) : NULL;
	env->door = bytes != 0 ? xcalloc(bytes / 8, 8) : NULL;
	env->shadow_mask = mask;
	env->shadow_epoch = 0;
	env->shadow_set_bits = 0;
	env->door_keys = 0;
	env->door_set_bits = 0;
	env->armed_walked = 0;
}

/** The map can carry the key: it hashes by the cmp_def. */
static bool
vy_cache_is_shadowed(struct vy_cache *cache, struct tuple *stmt)
{
	return !vy_stmt_is_cache_meta(stmt) &&
	       !cache->cmp_def->is_multikey &&
	       !cache->cmp_def->for_func_index;
}

/**
 * The map key: the statement's key hashed by the cache's
 * cmp_def, salted with the cache identity -- equal keys of
 * different indexes are different map keys -- and the hint.
 */
static uint64_t
vy_cache_shadow_hash(struct vy_cache *cache, struct vy_entry entry)
{
	uint64_t h = tuple_hash(entry.stmt, cache->cmp_def);
	h ^= (uint64_t)(uintptr_t)cache;
	h *= 0x9e3779b97f4a7c15;
	h ^= (uint64_t)entry.hint;
	h *= 0x9e3779b97f4a7c15;
	h ^= h >> 32;
	return h;
}

/** True if the array holds the key. */
static bool
vy_cache_shadow_probe(struct vy_cache_env *env, uint64_t *arr,
		      uint64_t h)
{
	for (int i = 0; i < 3; i++) {
		uint64_t bit = (h >> (21 * i)) & env->shadow_mask;
		if ((arr[bit >> 6] & (1UL << (bit & 63))) == 0)
			return false;
	}
	return true;
}

/** Stamp the key into the array; count the new bits. */
static uint64_t
vy_cache_shadow_stamp(struct vy_cache_env *env, uint64_t *arr,
		      uint64_t h)
{
	uint64_t set = 0;
	for (int i = 0; i < 3; i++) {
		uint64_t bit = (h >> (21 * i)) & env->shadow_mask;
		uint64_t word = 1UL << (bit & 63);
		if ((arr[bit >> 6] & word) == 0) {
			arr[bit >> 6] |= word;
			set++;
		}
	}
	return set;
}

/** The set density that wipes an array: a third of its bits. */
static bool
vy_cache_shadow_is_full(struct vy_cache_env *env, uint64_t set_bits)
{
	return set_bits * 3 >= env->shadow_mask + 1;
}

/** Flip the epochs: the old array is wiped and becomes young. */
static void
vy_cache_shadow_flip(struct vy_cache_env *env)
{
	env->shadow_set_bits = 0;
	env->armed_walked = 0;
	env->shadow_epoch ^= 1;
	memset(env->shadow[env->shadow_epoch], 0,
	       (env->shadow_mask + 1) / 8);
}

/**
 * Report a recorded read of the key; @return true if the key
 * is protected -- the caller's walk runs armed.
 */
static bool
vy_cache_shadow_record(struct vy_cache_env *env, uint64_t h)
{
	if (env->shadow_mask == 0)
		return false;
	uint64_t *young = env->shadow[env->shadow_epoch];
	uint64_t *old = env->shadow[env->shadow_epoch ^ 1];
	bool armed = vy_cache_shadow_probe(env, young, h) ||
		     vy_cache_shadow_probe(env, old, h) ||
		     vy_cache_shadow_probe(env, env->door, h);
	if (armed) {
		env->shadow_set_bits +=
			vy_cache_shadow_stamp(env, young, h);
		if (vy_cache_shadow_is_full(env, env->shadow_set_bits))
			vy_cache_shadow_flip(env);
		return true;
	}
	/*
	 * The doorkeeper's window is the resident row count: a
	 * key reappearing within it belongs to a set small
	 * enough to fit the cache, and is promoted. A longer
	 * recurring set -- a sweep by another name -- reappears
	 * after its stamps were wiped, and never promotes. The
	 * density bound is a backstop keeping the false
	 * positive rate down.
	 */
	env->door_keys++;
	env->door_set_bits += vy_cache_shadow_stamp(env, env->door, h);
	if (env->door_keys >= (uint64_t)MAX(env->tuple.rows, 1024) ||
	    vy_cache_shadow_is_full(env, env->door_set_bits)) {
		env->door_keys = 0;
		env->door_set_bits = 0;
		memset(env->door, 0, (env->shadow_mask + 1) / 8);
	}
	return false;
}

/**
 * True if either protected epoch holds the key: reprieved.
 * Both epochs count, so one flip does not strip a live set
 * of protection before its reads restamp the young epoch --
 * the price is that a dead set ages out over two flips.
 */
static bool
vy_cache_shadow_is_protected(struct vy_cache_env *env, uint64_t h)
{
	if (env->shadow_mask == 0)
		return false;
	return vy_cache_shadow_probe(env, env->shadow[0], h) ||
	       vy_cache_shadow_probe(env, env->shadow[1], h);
}

/**
 * Track the armed walks' progress; a revolution of armed
 * walking flips the epochs even below the density trigger,
 * so a dead protected set is aged out by the armed stream
 * competing with it.
 */
static void
vy_cache_shadow_armed_walk(struct vy_cache_env *env, size_t walked)
{
	if (env->shadow_mask == 0)
		return;
	env->armed_walked += walked;
	if (env->armed_walked >= env->mem_used)
		vy_cache_shadow_flip(env);
}

/* }}} shadow map */

/** Advance the eviction hand to the next cache, if there is one. */
static void
vy_cache_env_gc_next_cache(struct vy_cache_env *env)
{
	struct vy_cache *cache = env->gc_cache;
	assert(cache != NULL);
	if (rlist_next(&cache->in_gc_list) != &env->gc_list)
		env->gc_cache = rlist_next_entry(cache, in_gc_list);
	else if (!rlist_empty(&env->gc_list))
		env->gc_cache = rlist_first_entry(&env->gc_list,
						  struct vy_cache,
						  in_gc_list);
}

/** Add @a cache to the eviction ring, its hand state unset. */
static void
vy_cache_env_add_cache(struct vy_cache_env *env, struct vy_cache *cache)
{
	cache->gc_pos = vy_entry_none();
	rlist_add_tail(&env->gc_list, &cache->in_gc_list);
	if (env->gc_cache == NULL)
		env->gc_cache = cache;
}

/**
 * Remove @a cache from the eviction ring, advancing the hand
 * off it first if it stands there, and release its hand
 * position.
 */
static void
vy_cache_env_del_cache(struct vy_cache_env *env, struct vy_cache *cache)
{
	if (env->gc_cache == cache) {
		vy_cache_env_gc_next_cache(env);
		if (env->gc_cache == cache)
			env->gc_cache = NULL;
	}
	rlist_del(&cache->in_gc_list);
	if (cache->gc_pos.stmt != NULL)
		tuple_unref(cache->gc_pos.stmt);
	cache->gc_pos = vy_entry_none();
}

/**
 * The GCLOCK eviction hand: walk the entries of the current
 * cache, cooling used entries and evicting cold ones, to make
 * room for @a size bytes about to be admitted -- eviction is
 * driven by the admission volume. The walk is bounded: over a
 * cache full of hot entries it may reclaim nothing within its
 * budget, and the caller refuses the admission instead, see
 * vy_cache_insert(). When the hand falls off the end of a
 * cache's tree, it moves to the next cache.
 */
static void
vy_cache_env_gc(struct vy_cache_env *env, size_t size,
		bool armed)
{
	/*
	 * The budget is in bytes walked. It is proportional to
	 * the bytes being admitted or, when the quota was
	 * lowered, to one allocation extent. The standing
	 * overage may be huge -- the whole cache after a steep
	 * quota cut -- and walking it in one call would stall
	 * the TX thread, so a lowered quota is approached in
	 * extent-sized steps, driven by the admission traffic
	 * and the set_quota loop. A full circle over the cache
	 * ring ends the walk early: everything was visited once.
	 *
	 * The walk ends at the first room found, and the
	 * admission credit keeps that live: a fresh entry
	 * survives its first visit, so a walk over just-admitted
	 * entries evicts nothing and its admission is refused --
	 * the hand advances while the refused stream adds
	 * nothing, and no insertion pattern can outrun the hand
	 * and starve the rest of the ring of visits.
	 */
	size_t pressure = env->mem_used > env->mem_quota ?
			  VY_CACHE_TREE_EXTENT_SIZE : 0;
	ssize_t budget = MAX(size, pressure) * VY_CACHE_GC_FACTOR;
	ssize_t budget_start = budget;
	/*
	 * The weighting unit of the visit below: log2 of the
	 * mean resident tuple size, snapshot once per walk so
	 * every entry in one burst is judged by the same
	 * yardstick. The hand cools a tuple one lap, plus one
	 * lap per doubling of its size above the resident mean:
	 * an outlier sheds its heat within a few visits and
	 * cannot hog the cache, while a row at or below the mean
	 * cools one lap per visit -- a uniform population
	 * degrades to the unweighted hand.
	 */
	unsigned unit_shift = 63;
	if (env->tuple.rows > 0) {
		uint64_t mean = env->tuple.bytes / env->tuple.rows;
		unit_shift = 63 - bit_clz_u64(mean | 1);
	}
	struct vy_cache *first = env->gc_cache;
	struct vy_cache *cache = NULL;
	struct vy_cache_tree_iterator itr = vy_cache_tree_invalid_iterator();
	while (env->mem_used + size > env->mem_quota && budget > 0) {
		/*
		 * Dropped caches are perfect victims: no heat to
		 * consult, no links to maintain. Drain them before
		 * disturbing the live ring -- and even with no
		 * live ring left at all.
		 */
		if (!rlist_empty(&env->drain.queue)) {
			budget -= vy_cache_drain_step(env, budget);
			continue;
		}
		if (env->gc_cache == NULL)
			break;
		/*
		 * An invalid iterator means the hand is not
		 * positioned: on entry, and whenever it left a
		 * cache -- it enters the ring's current cache, at
		 * the saved position if one was kept.
		 */
		if (vy_cache_tree_iterator_is_invalid(&itr)) {
			cache = env->gc_cache;
			if (cache->gc_pos.stmt != NULL) {
				bool exact;
				itr = vy_cache_tree_lower_bound(
					cache->tree,
					cache->gc_pos, &exact);
				tuple_unref(cache->gc_pos.stmt);
				cache->gc_pos = vy_entry_none();
			} else {
				itr = vy_cache_tree_first(cache->tree);
			}
			/* An empty or consumed tree: move on. */
			if (vy_cache_tree_iterator_is_invalid(&itr))
				goto rotate;
			continue;
		}
		struct vy_cache_entry *node =
			vy_cache_tree_iterator_get_elem(cache->tree,
							&itr);
		budget -= vy_cache_entry_size(cache->is_primary, node);
		/*
		 * The visit: cool, then decide. Each entry cools
		 * the heat it lives by. The shared statement heat
		 * cools under the primary hand alone: it crosses
		 * every cached row exactly once per cycle, so a
		 * row loses one lap per cycle however many caches
		 * reference it.
		 */
		struct tuple *stmt = node->entry.stmt;
		bool is_tuple = !vy_stmt_is_cache_meta(stmt);
		if (is_tuple && cache->is_primary) {
			size_t ratio = tuple_size(stmt) >> unit_shift;
			unsigned dec = ratio > 0 ?
				64 - bit_clz_u64(ratio) : 1;
			vy_stmt_dec_heat(stmt, dec);
		} else if (node->heat > 0) {
			node->heat--;
		}
		/*
		 * The eviction threshold: normally zero -- only a
		 * fully cooled entry goes. A protected key's walk
		 * runs armed: the threshold rises to the admission
		 * credit, and any unprotected entry it covers
		 * yields its room, cooled or not, see the shadow
		 * map section.
		 */
		unsigned threshold = armed ? VY_CACHE_ADMIT_HEAT : 0;
		/*
		 * A meta entry and a secondary tuple entry live by
		 * their entry's own heat -- a secondary's interest
		 * in the row is local. A primary tuple entry is
		 * the row's home and lives by the row's heat,
		 * which reads through any index raise; its entry
		 * heat is never raised, read, or cooled. A
		 * maybe-stale entry is collected regardless of
		 * heat.
		 */
		bool stale = vy_stmt_maybe_stale(stmt, cache->is_primary);
		unsigned post_heat = is_tuple && cache->is_primary ?
			vy_stmt_heat(stmt) : node->heat;
		bool warm = !stale && post_heat > threshold;
		/*
		 * The reprieve: a protected entry is not evicted
		 * at any heat, on any walk -- its key is being
		 * read, whatever its heat says, see the shadow
		 * map section. A standing overage overrules it:
		 * a lowered quota must be reached whatever the
		 * map holds, or the reclaim would never end.
		 */
		if (!warm && !stale &&
		    env->mem_used <= env->mem_quota &&
		    vy_cache_is_shadowed(cache, stmt)) {
			uint64_t h = vy_cache_shadow_hash(cache,
							  node->entry);
			warm = vy_cache_shadow_is_protected(env, h);
		}
		if (warm) {
			vy_cache_tree_iterator_next(cache->tree,
						    &itr);
			/*
			 * The cache is exhausted: rotate now, or
			 * a walk ending here would restart this
			 * cache -- and revisit its tail -- on the
			 * next call.
			 */
			if (vy_cache_tree_iterator_is_invalid(&itr))
				goto rotate;
			continue;
		}
		/*
		 * Remember where to resume on the stack, without a
		 * reference: the next entry's statement is pinned
		 * by its own node, and nothing else evicts during
		 * the walk. The eviction may rebalance the tree, so
		 * the position is re-sought by key.
		 */
		struct vy_cache_tree_iterator next = itr;
		vy_cache_tree_iterator_next(cache->tree, &next);
		struct vy_entry resume = vy_entry_none();
		if (!vy_cache_tree_iterator_is_invalid(&next))
			resume = vy_cache_tree_iterator_get_elem(
					cache->tree, &next)->entry;
		/*
		 * Removing an entry widens its neighbors' pending
		 * links into a range their openers never read.
		 */
		vy_cache_clear_pending_links(cache, itr,
					     /*skip_curr=*/true);
		vy_stmt_counter_acct_tuple(&cache->stat.evict,
					   node->entry.stmt);
		vy_cache_evict_pos(cache, itr);
		if (resume.stmt != NULL) {
			bool exact;
			itr = vy_cache_tree_lower_bound(cache->tree,
							resume, &exact);
			continue;
		}
rotate:
		itr = vy_cache_tree_invalid_iterator();
		vy_cache_env_gc_next_cache(env);
		/* A full circle: every cache was visited once. */
		if (env->gc_cache == first)
			break;
	}
	if (armed && budget_start > budget)
		vy_cache_shadow_armed_walk(env, budget_start - budget);
	/* Save the hand position, referenced: the walk is over. */
	if (!vy_cache_tree_iterator_is_invalid(&itr)) {
		struct vy_cache_entry *node =
			vy_cache_tree_iterator_get_elem(cache->tree,
							&itr);
		cache->gc_pos = node->entry;
		tuple_ref(cache->gc_pos.stmt);
	}
}

void
vy_cache_env_set_quota(struct vy_cache_env *env, size_t quota)
{
	env->mem_quota = quota;
	vy_cache_env_shadow_resize(env);
	while (env->mem_used > env->mem_quota) {
		vy_cache_env_gc(env, 0, 0);
		if (env->mem_used <= env->mem_quota)
			break;
		/*
		 * Make sure we don't block other tx fibers
		 * for too long.
		 */
		fiber_sleep(0);
	}
}

/* }}} vy_cache_env */

/* {{{ vy_cache */

/**
 * A statement that is chain metadata rather than payload: a key
 * entry or a consumed DELETE. Metadata statements are private
 * copies, never shared with the other index's cache.
 */
static bool
vy_stmt_is_cache_meta(struct tuple *stmt)
{
	return vy_stmt_is_bound(stmt) ||
	       vy_stmt_type(stmt) == IPROTO_DELETE;
}

/** See the comment in vy_cache.h. */
bool
vy_stmt_maybe_stale(struct tuple *stmt, bool is_primary)
{
	return !is_primary && !vy_stmt_is_cache_meta(stmt) &&
	       !vy_stmt_has_flag(stmt, VY_STMT_PK_CACHED);
}

/**
 * The statement's admission into a cache. In the primary, a
 * tuple becomes the row's resident object -- the PK_CACHED bit
 * -- and receives the admission credit, one hand visit's worth
 * of heat. The cache's own metadata has no row and carries
 * neither.
 */
static void
vy_stmt_admit(struct tuple *stmt, bool is_primary)
{
	if (!is_primary || vy_stmt_is_cache_meta(stmt))
		return;
	vy_stmt_add_flag(stmt, VY_STMT_PK_CACHED);
	vy_stmt_raise_heat(stmt, VY_CACHE_ADMIT_HEAT);
}

/**
 * Clear the PK_CACHED bit as the statement's entry leaves the
 * cache. Only the primary owns the bit: a secondary entry
 * shares its statement with a live primary entry, and a
 * secondary departure must not touch it. Metadata never
 * carries the bit, so there is no need to test for it:
 * clearing is a no-op there.
 */
static void
vy_stmt_clear_pk_cached(struct tuple *stmt, bool is_primary)
{
	if (is_primary)
		vy_stmt_del_flag(stmt, VY_STMT_PK_CACHED);
}

/**
 * The statement's LSN is above the oldest read view: some
 * reader may still see the state it replaced. A plain key
 * entry, at LSN zero, never is.
 */
static bool
vy_stmt_is_above_horizon(struct tuple *stmt, struct vy_tx_manager *xm)
{
	return vy_stmt_lsn(stmt) > vy_tx_manager_horizon(xm);
}

/**
 * @return the bytes @a node accounts for: the element and its
 * statement.
 */
static size_t
vy_cache_entry_size(bool is_primary, const struct vy_cache_entry *node)
{
	size_t size = sizeof(*node);
	/*
	 * Tuples are shared between primary and secondary index
	 * cache so to avoid double accounting, we account only
	 * primary index tuples. Overhead entries are private
	 * copies, accounted in any cache.
	 */
	if (is_primary || vy_stmt_is_cache_meta(node->entry.stmt))
		size += tuple_size(node->entry.stmt);
	return size;
}

/** GCLOCK heat: a use gives the entry another lap. A tuple is
 * shared between the caches of its space, so a use heats the
 * statement itself -- the row's temperature, kept apart from
 * any one cache's view of it. A primary tuple entry lives by
 * the row's heat alone: its own entry heat is left untouched.
 */
static inline void
vy_cache_entry_touch(struct vy_cache_entry *node, bool is_primary)
{
	struct tuple *stmt = node->entry.stmt;
	if (!vy_stmt_is_cache_meta(stmt)) {
		vy_stmt_inc_heat(stmt);
		if (is_primary)
			return;
	}
	node->heat += node->heat < VY_CACHE_HEAT_MAX;
}

/**
 * Step the iterator one entry along @a direction and return the
 * entry it lands on, or NULL at the end of the tree.
 */
static struct vy_cache_entry *
vy_cache_tree_step(struct vy_cache_tree *tree,
		   struct vy_cache_tree_iterator *pos, int direction)
{
	if (direction > 0)
		vy_cache_tree_iterator_next(tree, pos);
	else
		vy_cache_tree_iterator_prev(tree, pos);
	if (vy_cache_tree_iterator_is_invalid(pos))
		return NULL;
	return vy_cache_tree_iterator_get_elem(tree, pos);
}

/** Account an element that has entered the tree. */
static void
vy_cache_acct_entry(struct vy_cache *cache,
		    const struct vy_cache_entry *el)
{
	tuple_ref(el->entry.stmt);
	cache->env->mem_used += vy_cache_entry_size(cache->is_primary, el);
	vy_cache_env_acct_tuple(cache->env, el->entry.stmt,
				cache->is_primary);
	vy_stmt_counter_acct_tuple(&cache->stat.count, el->entry.stmt);
	if (vy_stmt_is_cache_meta(el->entry.stmt))
		vy_stmt_counter_acct_tuple(&cache->stat.overhead,
					   el->entry.stmt);
}

/**
 * Release an element that has left the tree. Takes a copy of the
 * element, not a tree pointer: the tree slot is gone by now.
 */
static void
vy_cache_unacct_entry(struct vy_cache *cache, struct vy_cache_entry *el)
{
	vy_stmt_counter_unacct_tuple(&cache->stat.count, el->entry.stmt);
	if (vy_stmt_is_cache_meta(el->entry.stmt))
		vy_stmt_counter_unacct_tuple(&cache->stat.overhead,
					     el->entry.stmt);
	size_t size = vy_cache_entry_size(cache->is_primary, el);
	assert(cache->env->mem_used >= size);
	cache->env->mem_used -= size;
	vy_cache_env_unacct_tuple(cache->env, el->entry.stmt,
				  cache->is_primary);
	tuple_unref(el->entry.stmt);
}

void
vy_cache_create(struct vy_cache *cache, struct vy_cache_env *env,
		struct key_def *cmp_def, bool is_primary)
{
	cache->env = env;
	cache->cmp_def = cmp_def;
	cache->is_primary = is_primary;
	cache->version = 1;
	vy_cache_env_add_cache(env, cache);
	cache->tree = xmalloc(sizeof(*cache->tree));
	vy_cache_tree_create(cache->tree, cmp_def, &env->allocator);
}

void
vy_cache_destroy(struct vy_cache *cache)
{
	struct vy_cache_env *env = cache->env;
	vy_cache_env_del_cache(env, cache);
	if (vy_cache_tree_size(cache->tree) == 0) {
		vy_cache_tree_destroy(cache->tree);
		free(cache->tree);
		return;
	}
	/*
	 * Detach the tree instead of walking it here: a large
	 * cache would stall the TX thread for the whole walk.
	 * The eviction walk consumes dropped caches first, and a
	 * bounded batch is released inline, so most caches die
	 * right here and only a large one leaves a remainder.
	 */
	vy_cache_drain_add(env, cache->tree, cache->is_primary);
	cache->tree = NULL;
	vy_cache_drain_step(env, VY_CACHE_TREE_EXTENT_SIZE);
}

/**
 * Abort the pending links extended over the position at @a itr.
 *
 * Completion hops chain metadata, so the abortion must reach as
 * far as a completion can. Two fibers:
 *
 * - F1 scans, caches {10}, opens a pending link on it and
 *   yields, resolving its next result through the primary
 *   index:
 *
 *       {10} ..>
 *
 * - F2 scans for 17: it inserts its start bound [17]- and yields
 *   on a disk read:
 *
 *       {10} ..>   [17]- ..>
 *
 * - a write commits {20}. Both pending links are extended over
 *   it, and both must die. F2 later closes empty, and its bound
 *   stays: an aborted start bound is not taken back, see
 *   vy_cache_builder_break_link().
 *
 * If the abortion stopped at the written key's tree neighbors,
 * only [17]-'s pending link would die, and [17]- would shield
 * F1's: {10} is not adjacent to the write. F1 then wakes with
 * {30} -- its walk chose it before {20} existed, so the walk
 * saw nothing -- finds its pending link intact, and completes
 * the chain, hopping the bound:
 *
 *       {10} -> [17]- -> {30}
 *
 * The second link claims the range between [17]- and {30} holds
 * nothing, with committed {20} inside: every cache-served scan
 * takes the link's word, skips the deeper sources and loses the
 * row. The correct state after the write is no links at all:
 *
 *       {10}   [17]-   {30}
 *
 * So walk outward from the written position, aborting every
 * facing pending link, up to and including the first value
 * entry on each side. Stopping at the value is sound: a value
 * between a pending link's anchor and this position voids any
 * completion across it, and if that value is itself removed
 * later, its removal runs this abortion from its own position.
 *
 * @a skip_curr means the entry at @a itr is being removed, so
 * the right-side walk starts at its successor.
 */
static void
vy_cache_clear_pending_links(struct vy_cache *cache,
			     struct vy_cache_tree_iterator itr,
			     bool skip_curr)
{
	struct vy_cache_tree *tree = cache->tree;
	struct vy_cache_tree_iterator prev = itr;
	while (true) {
		vy_cache_tree_iterator_prev(tree, &prev);
		if (vy_cache_tree_iterator_is_invalid(&prev))
			break;
		struct vy_cache_entry *n =
			vy_cache_tree_iterator_get_elem(tree, &prev);
		if (n->pending_link_direction > 0)
			n->scan_id = 0;
		if (!vy_stmt_is_cache_meta(n->entry.stmt))
			break;
	}
	if (skip_curr)
		vy_cache_tree_iterator_next(tree, &itr);
	while (!vy_cache_tree_iterator_is_invalid(&itr)) {
		struct vy_cache_entry *n =
			vy_cache_tree_iterator_get_elem(tree, &itr);
		if (n->pending_link_direction < 0)
			n->scan_id = 0;
		if (!vy_stmt_is_cache_meta(n->entry.stmt))
			break;
		vy_cache_tree_iterator_next(tree, &itr);
	}
}

/**
 * Evict one entry: unlink its chain neighbors and delete it.
 * The neighbors' pending links are kept. That is only correct
 * when the removal hides no tuple: the entry is a bound key, a
 * DELETE, or a key proven dead through the primary index (see
 * vy_cache_evict()). A caller removing a live key must clear
 * the pending links first, or a formed link would span the
 * removed key, which may still be visible in some read view.
 */
static void
vy_cache_evict_pos(struct vy_cache *cache,
		   struct vy_cache_tree_iterator itr)
{
	struct vy_cache_entry *node, *prev;
	node = vy_cache_tree_iterator_get_elem(cache->tree, &itr);
	vy_cache_tree_iterator_prev(cache->tree, &itr);
	prev = vy_cache_tree_iterator_get_elem(cache->tree, &itr);
	if (prev != NULL) {
		/*
		 * Keep the predecessor linked across the removal
		 * when all of the following hold:
		 * 1) the removed entry is linked on both sides,
		 *    so the two links merge into one;
		 * 2) it is a bound key or a DELETE, so no row
		 *    disappears from the linked range;
		 * 3) its LSN is not above the oldest read view.
		 *    The LSN is that of the newest DELETE
		 *    recorded on the entry, zero if none, and a
		 *    reader older than it may still see one of
		 *    the deleted keys alive. Such a reader
		 *    ignores the links around an entry it cannot
		 *    see -- but once the entry is removed, there
		 *    is nothing left to ignore, and the merged
		 *    link would hide a key the reader is entitled
		 *    to. Break the link instead: the reader
		 *    descends and finds its key in the deeper
		 *    sources.
		 */
		prev->is_linked = prev->is_linked && node->is_linked &&
			vy_stmt_is_cache_meta(node->entry.stmt) &&
			vy_stmt_lsn(node->entry.stmt) <=
			vy_tx_manager_horizon(cache->env->xm);
	}
	cache->version++;
	struct vy_cache_entry victim = *node;
	vy_stmt_clear_pk_cached(victim.entry.stmt, cache->is_primary);
	vy_cache_tree_delete(cache->tree, victim);
	vy_cache_unacct_entry(cache, &victim);
}

/** See the comment in vy_cache.h. */
void
vy_cache_evict(struct vy_cache *cache, struct vy_entry entry)
{
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
	bool exact = false;
	struct vy_cache_tree_iterator pos =
		vy_cache_tree_lower_bound(cache->tree,
					  entry, &exact);
	if (!exact)
		return;
	/*
	 * The proven-dead verdict was made before the resolution
	 * yielded: the key may have been resurrected and re-cached
	 * as a newer live row meanwhile. The verdict applies to
	 * the version it was made about; on a version mismatch
	 * the entry is left alone.
	 */
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(cache->tree, &pos);
	if (vy_stmt_lsn(node->entry.stmt) != vy_stmt_lsn(entry.stmt))
		return;
	vy_cache_evict_pos(cache, pos);
}

/**
 * Reconcile a freshly inserted entry with the links formed
 * before it appeared, and return the surviving entry -- the
 * input, or the predecessor that absorbed it.
 *
 * The entry may land between two linked entries -- the
 * predecessor is linked to what is now the entry's successor,
 * so the range between them is known to hold no rows. The
 * cases, on the example of a cache holding tuples {10} and
 * {40} linked to each other:
 *
 * - A bound key lands between linked entries: it holds no row
 *   and no DELETE LSN, so the link stays true and the entry
 *   adds nothing -- it fuses away at once. E.g. a GE{20} scan
 *   places its start bound between {10} and {40}: the range is
 *   already known empty, the bound fuses away and the chain
 *   starts from {10} instead.
 * - A DELETE lands between linked entries: its LSN must live
 *   somewhere. If the predecessor is a bound key or a DELETE,
 *   the LSN moves there and the entry fuses into it: e.g.
 *   delete{20}, recorded right above the linked bound an
 *   earlier GE{15} scan left, raises that bound's LSN to its
 *   own and vanishes -- one entry now covers both deletes.
 * - The DELETE's predecessor is a tuple: an LSN cannot attach
 *   to a tuple. With no reader below the DELETE's LSN the
 *   entry protects nobody and fuses away. With such a reader
 *   the entry must not stay either. E.g. {10} is linked to a
 *   bound key at LSN 2000, delete{15} at 2000 and delete{20}
 *   at 1000 fused into it, and a reader at read view 1500
 *   records delete{20} -- the one DELETE it sees. Kept as an
 *   entry, {20} would split the link in two, and the {10}-{20}
 *   half would keep the link flag with no LSN to guard it: the
 *   recording reader itself would then cross from {20} to {10}
 *   and miss {15}, alive at 1500. The link stays whole and
 *   keeps gating every crossing by its ends: the DELETE fuses
 *   into its successor when that is a bound key or a DELETE,
 *   and is refused otherwise.
 * - A tuple lands between linked entries: the link promised an
 *   empty range and is now false -- destroy both halves. In
 *   practice this cannot happen: committing {20} invalidates
 *   the cache at its key at prepare, removing any link over it,
 *   so this is a defense, not a protocol.
 * - No link spans the landing spot: nothing is known about the
 *   range, and the entry stays. E.g. the GE{20} bound next to a
 *   cached but unlinked {10} remains and starts its own chain;
 *   delete{20} in the same spot remains too -- a DELETE only
 *   reaches this insert while some reader still needs it (one
 *   at or below the horizon is refused before insertion, see
 *   vy_cache_builder_add_delete()), and here there is no entry
 *   to hand its LSN to. Either way the entry is not left
 *   dangling: the chain joins it right after, see
 *   vy_cache_link().
 *
 * @param cache the cache.
 * @param[in,out] pos the entry's tree position, repositioned to
 *        the survivor when the entry fuses away.
 * @param node the entry.
 * @retval the surviving entry.
 * @retval NULL the entry was refused and removed.
 */
static struct vy_cache_entry *
vy_cache_relink(struct vy_cache *cache, struct vy_cache_tree_iterator *pos,
		struct vy_cache_entry *node)
{
	struct vy_cache_tree_iterator it = *pos;
	vy_cache_tree_iterator_prev(cache->tree, &it);
	struct vy_cache_entry *below =
		vy_cache_tree_iterator_get_elem(cache->tree, &it);
	if (below == NULL || !below->is_linked)
		return node;
	if (!vy_stmt_is_cache_meta(node->entry.stmt)) {
		/* A tuple destroys both halves. */
		below->is_linked = false;
		node->is_linked = false;
		cache->version++;
		return node;
	}
	if (vy_stmt_is_cache_meta(below->entry.stmt)) {
		int64_t lsn = vy_stmt_lsn(node->entry.stmt);
		if (vy_stmt_lsn(below->entry.stmt) < lsn)
			vy_stmt_set_lsn(below->entry.stmt, lsn);
	} else if (vy_stmt_lsn(node->entry.stmt) >
		   vy_tx_manager_horizon(cache->env->xm)) {
		/*
		 * A DELETE whose predecessor is a tuple, which
		 * cannot carry an LSN. Keeping the DELETE as an
		 * entry would split the claim in two, and a
		 * reader crossing only the tuple's half would
		 * never pass the entry guarding the whole claim.
		 * Instead the DELETE fuses into its successor
		 * when the successor is a bound key or a DELETE:
		 * the successor's LSN is raised to the newer of
		 * the two, the new entry is removed, and the
		 * claim stays whole, gated by the successor for
		 * this key too. E.g. a reverse scan paginated at
		 * {21} fused delete{20} at LSN 1500 and
		 * delete{15} at LSN 2000 into its start bound
		 * and linked the tuple {10} to it. A reader at
		 * read view 1700 does not see the bound at 2000,
		 * descends, and records the one DELETE it does
		 * see, delete{20}: the record lands between {10}
		 * and the bound and fuses back into it -- the
		 * bound's LSN, already above, needs no raise --
		 * and the {10}-bound claim stays whole. A raise
		 * is safe: it only narrows the successor's
		 * visibility, sending more readers to the
		 * sources. When the successor is a tuple too,
		 * there is nowhere to put the guard: the DELETE
		 * is refused and only the recording scan's own
		 * chain breaks -- the claim it crossed stays as
		 * it was.
		 */
		struct vy_cache_tree_iterator next_pos = *pos;
		struct vy_cache_entry *next =
			vy_cache_tree_step(cache->tree, &next_pos, 1);
		int64_t lsn = vy_stmt_lsn(node->entry.stmt);
		bool fuse_next = next != NULL &&
			vy_stmt_is_cache_meta(next->entry.stmt);
		if (fuse_next && vy_stmt_lsn(next->entry.stmt) < lsn)
			vy_stmt_set_lsn(next->entry.stmt, lsn);
		struct vy_entry surv = fuse_next ?
			next->entry : vy_entry_none();
		struct vy_cache_entry victim = *node;
		vy_cache_tree_delete(cache->tree, victim);
		vy_cache_unacct_entry(cache, &victim);
		cache->version++;
		if (!fuse_next)
			return NULL;
		bool exact;
		*pos = vy_cache_tree_lower_bound(cache->tree, surv, &exact);
		assert(exact);
		return vy_cache_tree_iterator_get_elem(cache->tree, pos);
	}
	/*
	 * The entry is redundant and fuses into the predecessor,
	 * whose link, unchanged, now spans the removed entry's
	 * key directly. The removal may rebalance the tree:
	 * remember the survivor by value and re-seek it.
	 */
	struct vy_entry surv = below->entry;
	struct vy_cache_entry victim = *node;
	vy_cache_tree_delete(cache->tree, victim);
	vy_cache_unacct_entry(cache, &victim);
	cache->version++;
	bool exact;
	*pos = vy_cache_tree_lower_bound(cache->tree, surv, &exact);
	assert(exact);
	return vy_cache_tree_iterator_get_elem(cache->tree, pos);
}

#ifndef NDEBUG
/** LINKLOG: trace formed links, enabled with VY_CACHE_TRACE. */
static bool
vy_linklog_is_enabled(void)
{
	char buf[2];
	return getenv_safe("VY_CACHE_TRACE", buf, sizeof(buf)) != NULL;
}

/**
 * LINKLOG: report a builder event, so a violation found later
 * can be replayed from the forming scan's exact input stream.
 */
static void
vy_linklog_event(struct vy_cache_builder *builder, const char *what,
		 struct vy_entry e, int64_t lsn, int is_stale)
{
	if (!vy_linklog_is_enabled())
		return;
	fprintf(stderr, "EV %s cache=%p scan=%llu vlsn=%lld stale=%d "
		"lsn=%lld stmt=%.60s\n", what, (void *)builder->cache,
		(unsigned long long)builder->scan_id,
		(long long)(**builder->rv).vlsn, is_stale,
		(long long)lsn,
		e.stmt == NULL ? "-" : vy_stmt_str(e.stmt));
}

/**
 * LINKLOG: report a link the walk has just completed, so a
 * violation found later can be traced back to the scan that
 * stated it.
 */
static void
vy_linklog_link(struct vy_cache_builder *builder,
		struct vy_cache_entry *lo, struct vy_cache_entry *hi)
{
	if (!vy_linklog_is_enabled())
		return;
	fprintf(stderr, "LINK cache=%p scan=%llu type=%d key=%.40s "
		"lo=%.60s hi=%.60s\n", (void *)builder->cache,
		(unsigned long long)builder->scan_id, builder->order,
		vy_stmt_str(builder->key.stmt),
		vy_stmt_str(lo->entry.stmt),
		vy_stmt_str(hi->entry.stmt));
}
#endif

/**
 * Complete the link from the chain's previous entry @a prev, if
 * any, to the entry at @a pos, consuming the pending link opened
 * when @a prev became the frontier.
 * @param cache the cache.
 * @param pos the entry's tree position.
 * @param node the entry.
 * @param builder the chain being built.
 * @param prev the chain's previous entry to link from, none to
 *        only open the pending link.
 */
static void
vy_cache_link(struct vy_cache *cache, struct vy_cache_tree_iterator pos,
	      struct vy_cache_entry *node, struct vy_cache_builder *builder,
	      struct vy_entry prev)
{
	assert(builder != NULL);
	if (prev.stmt == NULL)
		return;
	/*
	 * The entry may have fused into the chain's previous
	 * entry itself: e.g. an end key placed right above the
	 * last result inside a linked range. The chain already
	 * reaches past the searched range then, and there is
	 * nothing to link. The comparison minds the hint: two
	 * multikey entries share one statement and must link.
	 */
	if (vy_entry_is_equal(node->entry, prev))
		return;
	int direction = iterator_direction(builder->order);
	uint64_t scan_id = builder->scan_id;
	/*
	 * The link-to-be spans (prev, curr). A tuple strictly between
	 * them voids it -- it cannot appear on this branch, since a
	 * tuple cached by another reader is visible to this one and
	 * would have been the reader's result instead. Key entries
	 * and DELETEs of other readers may legitimately sit in
	 * between though: neither holds a live row, and the reader
	 * observed the whole span, so the chain links to them, not
	 * across them -- the walk connects every adjacent pair on
	 * the way. Anything else skips the link. The walk also
	 * locates prev, by key: no separate lookup, and the entry
	 * may meanwhile be backed by a newer statement -- an update
	 * that leaves the key unchanged replaces the cached row,
	 * see vy_cache_insert().
	 */
	struct vy_cache_entry *prev_node = NULL;
	int steps = 0;
	struct vy_cache_tree_iterator walk = pos;
	while (true) {
		struct vy_cache_entry *mid =
			vy_cache_tree_step(cache->tree, &walk,
					   -direction);
		if (mid == NULL)
			return;
		steps++;
		/*
		 * Compare by pointer for fast path, since the
		 * builder preserves the original statements on
		 * repeat scans. Resort to by-value comparison:
		 * the entry may be backed by a different object,
		 * e.g. a bound evicted and re-inserted as a fresh
		 * copy while the reader was away. The walk then
		 * still stops at the true predecessor, whose
		 * stamp decides whether the link may form.
		 */
		if (mid->entry.stmt == prev.stmt ||
		    vy_bound_cmp(mid->entry, prev, cache->cmp_def) == 0) {
			prev_node = mid;
			/*
			 * Prev found one step away, with nothing
			 * in between: if the pair is already
			 * linked, we are in a re-scan, no need to
			 * link an existing range again.
			 */
			if (steps == 1 && (direction > 0 ?
					   mid : node)->is_linked)
				return;
			break;
		}
		if (!vy_stmt_is_cache_meta(mid->entry.stmt))
			return;
	}
	/*
	 * Complete the pending link opened when prev was cached.
	 * The link forms only on this scan's own id, and the id
	 * alone decides. A write abort zeroes it; a concurrent
	 * scan's re-stamp replaces it. A mismatch therefore
	 * means a write got there first, or another scan
	 * re-stamped the entry after a write this one may have
	 * missed. Completion is a pure read: the stale id lies
	 * around until the next stamp, and no correctness rests
	 * on clearing it.
	 */
	if (prev_node->scan_id != scan_id)
		return;
	assert(prev_node->pending_link_direction == direction);
	/*
	 * Link every adjacent pair on the path: no tuples exist
	 * between prev and curr, so none exist between any two
	 * consecutive entries either. The walk stands on prev;
	 * walk it back to curr. Some pairs may already be linked
	 * -- e.g. the finalizing entry carries an earlier,
	 * narrower link through an intermediate bound key -- so
	 * bump the version only when a link is actually created.
	 *
	 * A DELETE crossed on the way inherits prev's LSN, when
	 * prev is a DELETE with a higher one: prev carries the
	 * newest LSN of the deletes fused into it, and some of
	 * their keys may fall in the crossed entry's own gaps.
	 * E.g. delete{20} is cached by an earlier scan, and a
	 * scan fusing delete{10} and delete{25} carries the LSN
	 * of delete{25} on its key {10} entry. Key 25 falls into
	 * the gap past {20}, so it is guarded by the {20}
	 * entry's visibility. That entry's LSN must rise too, or
	 * a reader still entitled to see the row of key 25 would
	 * observe the links around {20} and miss it.
	 */
	int64_t max_lsn = vy_stmt_is_cache_meta(prev_node->entry.stmt) ?
			  vy_stmt_lsn(prev_node->entry.stmt) : 0;
	struct vy_cache_entry *from = prev_node;
	bool changed = false;
	while (!vy_cache_tree_iterator_is_equal(cache->tree,
						&walk, &pos)) {
		struct vy_cache_entry *to =
			vy_cache_tree_step(cache->tree, &walk,
					   direction);
		assert(to != NULL);
		if (!vy_cache_tree_iterator_is_equal(cache->tree,
						     &walk, &pos) &&
		    vy_stmt_is_cache_meta(to->entry.stmt) &&
		    vy_stmt_lsn(to->entry.stmt) < max_lsn)
			vy_stmt_set_lsn(to->entry.stmt, max_lsn);
		struct vy_cache_entry *lo = direction > 0 ? from : to;
		if (!lo->is_linked) {
			lo->is_linked = true;
			changed = true;
#ifndef NDEBUG
			vy_linklog_link(builder, direction > 0 ? from : to,
					direction > 0 ? to : from);
#endif
		}
		from = to;
	}
	if (changed)
		cache->version++;
}

/**
 * Put an entry into the cache and weave it into the structure
 * around it: find or insert the tree entry, reconcile a fresh
 * one with the links formed before it appeared, and join it to
 * the chain being built, if any (see vy_cache_link()).
 * @param cache the cache to populate.
 * @param curr the statement to cache: a data statement or a
 *        bound key.
 * @param builder the chain being built, NULL outside a chain.
 *        The surviving entry becomes the chain's frontier.
 * @param prev the chain's previous entry to link from, none to
 *        only open the pending link.
 * @retval the surviving entry.
 * @retval NULL the statement cannot enter the cache.
 */
static struct vy_cache_entry *
vy_cache_insert(struct vy_cache *cache, struct vy_entry curr,
		struct vy_cache_builder *builder, struct vy_entry prev)
{
	assert(curr.stmt != NULL);
	assert(prev.stmt == NULL ||
	       !vy_lsn_is_prepared(vy_stmt_lsn(prev.stmt)));
	/* Every user-facing entry point gates on a disabled cache. */
	assert(cache->env->mem_quota != 0);

	/*
	 * Prepared, but not committed data never enters the cache:
	 * neither statements from a tx write set (lsn == INT64_MAX)
	 * nor prepared ones (pseudo-LSN). Prepared data is
	 * retractable, and a retract cannot reach into the cache:
	 * commit rewrites the LSN of the memory-level statement,
	 * not of a cached copy, and commit does not invalidate the
	 * cache (only prepare and rollback do). This stays
	 * consistent at every isolation level because prepare also
	 * invalidates the committed data and the links its write
	 * covers: the cache serves neither the prepared tuple nor
	 * anything it overwrote. Both a read-committed and a
	 * read-confirmed reader find a gap and descend to the
	 * memory level, where the first sees the prepared version
	 * and the second skips it. Every caller refuses prepared
	 * data before reaching this point.
	 */
	assert(!vy_lsn_is_prepared(vy_stmt_lsn(curr.stmt)));
	assert(vy_stmt_is_bound(curr.stmt) ||
	       vy_stmt_type(curr.stmt) == IPROTO_INSERT ||
	       vy_stmt_type(curr.stmt) == IPROTO_REPLACE ||
	       vy_stmt_type(curr.stmt) == IPROTO_DELETE);

	/*
	 * A secondary cache admits a tuple only while the row is
	 * resident in the primary cache: invalidation reaches a
	 * secondary entry's row through the primary entry, so the
	 * secondary entry must never outlive it. The read path
	 * admits the resolved row into the primary first, and the
	 * statement arriving here is that very object, so the bit
	 * is the admission verdict.
	 */
	if (vy_stmt_maybe_stale(curr.stmt, cache->is_primary))
		return NULL;

	struct vy_cache_entry el;
	el.entry = curr;
	el.bits = 0;
	el.heat = VY_CACHE_ADMIT_HEAT;
	/*
	 * Make room for the entry: eviction is driven by the
	 * admission volume, and admission must not outrun
	 * eviction -- when the bounded walk cannot reclaim
	 * enough from caches full of hot entries, a fresh
	 * admission is refused below.
	 *
	 * The read is recorded in the shadow map; a protected
	 * key's walk runs armed -- eviction goes by recency, not
	 * heat, see the shadow map section.
	 */
	size_t size = vy_cache_entry_size(cache->is_primary, &el);
	bool armed = false;
	if (vy_cache_is_shadowed(cache, curr.stmt)) {
		uint64_t hash = vy_cache_shadow_hash(cache, curr);
		armed = vy_cache_shadow_record(cache->env, hash);
	}
	vy_cache_env_gc(cache->env, size, armed);
	bool exact;
	struct vy_cache_tree_iterator pos;
	if (vy_cache_tree_find_or_insert(cache->tree, el, &pos,
					 &exact) != 0) {
		/* memory error, let's live without a cache */
		return NULL;
	}
	assert(!vy_cache_tree_iterator_is_invalid(&pos));
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(cache->tree, &pos);
	if (exact) {
		/*
		 * A repeat read keeps the incumbent. It is at
		 * least as recent, and the cache mirrors
		 * committed data, so it holds the same row -- or
		 * a DELETE whose absence claim covers the
		 * incoming one. Keeping it leaves the link state,
		 * the accounting, the references and the cache
		 * version untouched. Concurrent cache iterators
		 * stay valid across re-reads, and the entry's
		 * statement identity is stable. A bound
		 * key, inserted at LSN zero, is always kept: the
		 * shared key statement holds its identity for
		 * the whole entry lifetime.
		 *
		 * Only a newer statement replaces the incumbent.
		 * A secondary index is not written -- and its
		 * cache not invalidated -- when an update leaves
		 * the indexed fields unchanged, so the
		 * incumbent's resolved tuple may carry stale
		 * non-indexed fields. The replacement keeps the
		 * whole link state, heat included: the key set is
		 * unchanged, and the reader's use of the key was
		 * already counted when the cache iterator yielded
		 * the incumbent as a candidate. A replace is
		 * never refused: the incumbent is stale and must
		 * go. The gc above reclaimed room for the whole
		 * new entry while a replace only adds its size
		 * delta, so any transient overshoot is bounded
		 * and the pressure budget corrects it.
		 */
		if (vy_stmt_lsn(curr.stmt) >
		    vy_stmt_lsn(node->entry.stmt)) {
			struct vy_cache_entry replaced;
			replaced.entry = vy_entry_none();
			struct vy_cache_tree_iterator unused;
			if (vy_cache_tree_insert_get_iterator(
					cache->tree, el, &replaced,
					&unused) == 0) {
				assert(replaced.entry.stmt != NULL);
				node = vy_cache_tree_iterator_get_elem(
					cache->tree, &pos);
				node->bits = replaced.bits;
				vy_stmt_clear_pk_cached(replaced.entry.stmt,
							cache->is_primary);
				vy_stmt_admit(curr.stmt,
					      cache->is_primary);
				vy_cache_acct_entry(cache, node);
				vy_cache_unacct_entry(cache, &replaced);
				cache->version++;
				vy_stmt_counter_acct_tuple(&cache->stat.put,
							   curr.stmt);
			}
		}
	} else if (cache->env->mem_used + size <= cache->env->mem_quota) {
		vy_cache_acct_entry(cache, node);
		cache->version++;
		vy_stmt_admit(curr.stmt, cache->is_primary);
		if (!vy_stmt_is_bound(curr.stmt))
			vy_stmt_counter_acct_tuple(&cache->stat.put,
						   curr.stmt);
		node = vy_cache_relink(cache, &pos, node);
		if (node == NULL)
			return NULL;
	} else {
		/*
		 * The caches stayed full of hot entries: refuse
		 * the admission. The insert and the delete moved
		 * the tree under concurrent iterators even though
		 * the key set is back intact, so the version must
		 * move too.
		 */
		vy_cache_tree_delete(cache->tree, el);
		cache->version++;
		cache->stat.reject++;
		return NULL;
	}
	if (builder == NULL)
		return node;
	vy_cache_link(cache, pos, node, builder, prev);
	vy_cache_builder_on_read(builder, node, pos);
	return node;
}

/** See the comment in vy_cache.h. */
void
vy_cache_add(struct vy_cache *cache, struct vy_entry entry)
{
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
	/* A NULL point result is not recorded in any form. */
	if (entry.stmt == NULL)
		return;
	if (vy_stmt_has_flag(entry.stmt, VY_STMT_STALE))
		return;
	/* See the prepared-data contract in vy_cache_insert(). */
	if (vy_lsn_is_prepared(vy_stmt_lsn(entry.stmt)))
		return;
	vy_cache_insert(cache, entry, /*builder=*/NULL, vy_entry_none());
}

struct vy_entry
vy_cache_get(struct vy_cache *cache, struct vy_entry key)
{
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return vy_entry_none();
	bool exact = false;
	struct vy_cache_tree_iterator pos =
		vy_cache_tree_lower_bound(cache->tree, key, &exact);
	if (!exact)
		return vy_entry_none();
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(cache->tree, &pos);
	/* The point encounter collects a maybe-stale entry. */
	if (vy_stmt_maybe_stale(node->entry.stmt, cache->is_primary)) {
		vy_cache_clear_pending_links(cache, pos,
					     /*skip_curr=*/true);
		vy_cache_evict_pos(cache, pos);
		return vy_entry_none();
	}
	vy_cache_entry_touch(node, cache->is_primary);
	assert(!vy_stmt_has_flag(node->entry.stmt, VY_STMT_STALE));
	return node->entry;
}

/**
 * Invalidate the cache after a write. The cache is a mirror of
 * the committed data, so a write makes the mirror of its key
 * stale. Called at statement prepare (@a rollback false) and at
 * the rollback of a prepared statement (@a rollback true).
 *
 * In every case the pending links facing the written key die:
 * their openers may already be past the key and would complete
 * a link over a write they never observed. The rest is a
 * three-row decision:
 * - the written key is cached: the stale entry is removed and
 *   its links die with it, whatever the write is;
 * - the key is a gap and the write only shrinks visibility (a
 *   committed DELETE): links spanning the key stay true and
 *   survive;
 * - the key is a gap and the write is anything else: the link
 *   spanning it is destroyed.
 *
 * @param cache the cache.
 * @param entry the written statement.
 * @param rollback the write is a retraction of a prepared
 *                 statement rather than a newly prepared one.
 * @param[out] found if not NULL, set to the removed cached
 *                   statement, referenced; untouched when the
 *                   written key was not cached.
 */
static void
vy_cache_invalidate(struct vy_cache *cache, struct vy_entry entry,
		    bool rollback, struct vy_entry *found)
{
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
	bool exact = false;
	struct vy_cache_tree_iterator itr;
	itr = vy_cache_tree_lower_bound(cache->tree,
					entry, &exact);
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(cache->tree, &itr);
	/*
	 * lower_bound follows the STL convention: the first element
	 * that is not less than the key -- the lower end of the
	 * equal range, not an element below the key.
	 * There are three cases possible
	 * (1) there's a value in cache that is equal to entry.
	 *   ('exact' == true, 'node' points the equal value in cache)
	 * (2) there's no value in cache that is equal to entry, and lower_bound
	 *   returned the next record.
	 *   ('exact' == false, 'node' points to the next record in cache)
	 * (3) there's no value in cache that is equal to entry, and lower_bound
	 *   returned invalid iterator, so there's no bigger value.
	 *   ('exact' == false, 'node' == NULL)
	 */
	assert(!exact || node != NULL);

	/*
	 * In every case the pending links facing the write die:
	 * their openers may already be past the written key and
	 * would complete a link over a write they never observed.
	 */
	vy_cache_clear_pending_links(cache, itr, /*skip_curr=*/exact);

	/*
	 * A committed DELETE only shrinks visibility: one more
	 * dead key cannot falsify a link's claim that no tuples
	 * lie in between. Any other write -- a new tuple, or a
	 * retraction, which may grow visibility back -- refutes
	 * the link spanning it.
	 */
	bool keep_link = !rollback &&
			 vy_stmt_type(entry.stmt) == IPROTO_DELETE;
	if (!exact && keep_link)
		return;

	/* The link of the write's predecessor spans the write. */
	vy_cache_tree_iterator_prev(cache->tree, &itr);
	struct vy_cache_entry *prev =
		vy_cache_tree_iterator_get_elem(cache->tree, &itr);

	if (!exact) {
		if (prev != NULL && prev->is_linked) {
			prev->is_linked = false;
			cache->version++;
		}
		return;
	}

	/*
	 * The written key is cached: the stale entry goes away and
	 * both adjacent links die with their endpoint -- the
	 * predecessor's and the entry's own. A single version bump
	 * covers it all: readers only compare versions for
	 * equality.
	 */
	if (prev != NULL)
		prev->is_linked = false;
	struct vy_cache_entry victim = *node;
	assert(vy_stmt_type(victim.entry.stmt) == IPROTO_INSERT ||
	       vy_stmt_type(victim.entry.stmt) == IPROTO_REPLACE ||
	       vy_stmt_type(victim.entry.stmt) == IPROTO_DELETE);
	if (found != NULL) {
		*found = victim.entry;
		tuple_ref(victim.entry.stmt);
	}
	vy_stmt_counter_acct_tuple(&cache->stat.invalidate,
				   victim.entry.stmt);
	vy_stmt_clear_pk_cached(victim.entry.stmt, cache->is_primary);
	vy_cache_tree_delete(cache->tree, victim);
	vy_cache_unacct_entry(cache, &victim);
	cache->version++;
}

void
vy_cache_on_write(struct vy_cache *cache, struct vy_entry entry,
		  struct vy_entry *found)
{
	vy_cache_invalidate(cache, entry, /*rollback=*/false, found);
}

void
vy_cache_on_rollback(struct vy_cache *cache, struct vy_entry entry)
{
	vy_cache_invalidate(cache, entry, /*rollback=*/true, NULL);
}

/* }}} vy_cache */

/* {{{ vy_cache_builder */

/** See the comment in vy_cache.h. */
void
vy_cache_builder_create(struct vy_cache_builder *builder,
			struct vy_cache *cache, enum iterator_type order,
			struct vy_entry key, struct vy_entry last)
{
	builder->cache = cache;
	builder->order = order;
	builder->key = key;
	tuple_ref(key.stmt);
	builder->last = vy_entry_none();
	builder->last_pos = vy_cache_tree_invalid_iterator();
	builder->chain_length = 0;
	builder->scan_id = vy_cache_env_new_scan_id(cache->env);
	builder->end_bound = vy_entry_none();
	builder->start_bound = vy_entry_none();
	/*
	 * A disabled cache serves and records nothing: skip the
	 * bound allocation every range read would otherwise pay.
	 * The seek treats the missing bound as "the cache serves
	 * nothing", see vy_cache_iterator_seek().
	 */
	if (cache->env->mem_quota == 0)
		return;
	/*
	 * A fresh scan starts at its search key's bound per the
	 * iterator type. A resumed scan -- a paginated read
	 * continuing after the position returned by the previous
	 * page, e.g.
	 *
	 *   res, pos = s:select(key, {limit = 10, fetch_pos = true})
	 *   res = s:select(key, {limit = 10, after = pos})
	 *
	 * -- starts strictly past the returned position (an
	 * exclusive restart: GT forward, LT in reverse), so the
	 * second page's chain begins where the first page's ended
	 * and a repeat of the second page is served from the
	 * cache. The range before the start bound was not observed,
	 * and nothing is claimed about it.
	 */
	struct vy_entry start_key = key;
	enum iterator_type start_order = order;
	if (last.stmt != NULL) {
		start_key = last;
		start_order = iterator_resume_order(order);
	}
	builder->start_bound = vy_bound_new(start_key, start_order);
	/*
	 * A full-key EQ does not need a lower or an upper bound
	 * to identify its range, since it matches at most one
	 * row: an exact cache hit is the whole answer
	 * structurally. Uniqueness of the index plays no part
	 * here. cmp_def orders a secondary index by its key
	 * parts with the primary key parts appended, so a key
	 * full by cmp_def is a unique position in any index. A
	 * plain secondary key, even a unique one, has fewer
	 * parts than its cmp_def and builds a bounded chain.
	 * E.g. space:pairs(pk, {iterator = 'EQ'}) on the
	 * primary index lands here, and the builder degenerates
	 * to admitting the point.
	 */
	if ((order == ITER_EQ || order == ITER_REQ) &&
	    vy_stmt_is_full_key(key.stmt, cache->cmp_def))
		return;
	/*
	 * The key the chain will end at: for EQ, the far side of
	 * the searched key; for a range reader, the far end of
	 * the key space -- +inf forward, -inf in reverse. A bound
	 * at the scan's last result would witness nothing: the
	 * range between a full key and its own bound is empty by
	 * construction, and a repeat scan would find no witness
	 * for the tail it must re-verify. The bound is made here
	 * and inserted at close, when a completed walk links its
	 * last result to it (see vy_cache_builder_close()).
	 */
	struct vy_entry end_key = order == ITER_EQ || order == ITER_REQ ?
				  key : cache->env->empty_key;
	builder->end_bound =
		vy_bound_new(end_key, iterator_resume_order(order));
	/*
	 * The start bound may fuse into the entry the chain grows
	 * from when it lands in a linked range; either way the
	 * survivor carries the chain's pending link, opened
	 * before the reader can yield: a write between the key
	 * entry and the first added point destroys it via
	 * invalidation, and the first added point links back only
	 * if it survived.
	 */
	if (builder->start_bound.stmt != NULL)
		vy_cache_insert(cache, builder->start_bound, builder,
				vy_entry_none());
}

/** See the comment in vy_cache.h. */
void
vy_cache_builder_destroy(struct vy_cache_builder *builder)
{
	tuple_unref(builder->key.stmt);
	if (builder->start_bound.stmt != NULL)
		tuple_unref(builder->start_bound.stmt);
	if (builder->end_bound.stmt != NULL)
		tuple_unref(builder->end_bound.stmt);
	vy_cache_builder_break_link(builder);
}

/**
 * Record the end of matches: insert the end bound made at the
 * builder's creation and link the last added point to it,
 * consuming its pending link.
 */
static void
vy_cache_builder_close(struct vy_cache_builder *builder)
{
	/*
	 * A disabled cache completes nothing. A tainted tail
	 * never reaches this point: vy_cache_builder_add()
	 * breaks the chain instead of closing it.
	 */
	if (builder->cache->env->mem_quota == 0) {
		vy_cache_builder_break_link(builder);
		return;
	}
	/*
	 * The end bound was made at creation (a full-key EQ has
	 * none, see vy_cache_builder_create()); inserting it is
	 * legitimate only here, at close: a range reader is
	 * closed only when it ran out of tuples (one stopped by
	 * a limit is abandoned and never adds a none result), so
	 * the whole tail was observed and the link's claim is
	 * true.
	 *
	 * A reader with no results ends at its own bound key: no
	 * link. A broken chain has nothing to link from. The
	 * frontier may be a bound key: a trailing DELETE fuses
	 * into an adjacent bound on insertion. The bound then
	 * carries the DELETE's LSN and is the entry the chain
	 * ends from.
	 */
	struct vy_entry last = builder->last;
	if (builder->chain_length > 0 && last.stmt != NULL &&
	    builder->end_bound.stmt != NULL) {
		assert(!vy_lsn_is_prepared(vy_stmt_lsn(last.stmt)));
		vy_cache_insert(builder->cache, builder->end_bound,
				builder, last);
	}
	vy_cache_builder_break_link(builder);
}

/** See the comment in vy_cache.h. */
void
vy_cache_builder_add(struct vy_cache_builder *builder, struct vy_entry curr,
		     bool is_stale)
{
	struct vy_cache *cache = builder->cache;
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
#ifndef NDEBUG
	vy_linklog_event(builder, curr.stmt == NULL ? "END" : "ADD", curr,
			 curr.stmt == NULL ? 0 : vy_stmt_lsn(curr.stmt),
			 is_stale);
#endif
	/* The end of matches: not an admission, close the chain. */
	if (curr.stmt == NULL) {
		/*
		 * A tainted tail -- the walk from the last result
		 * to the end of matches crossed a statement above
		 * the read view -- ends the chain without an end
		 * bound: the tail was not proven empty for every
		 * reader.
		 */
		if (is_stale)
			vy_cache_builder_break_link(builder);
		else
			vy_cache_builder_close(builder);
		return;
	}
	/*
	 * The result was served from the cache over observed
	 * links: vy_cache_builder_on_read() has already made it the
	 * frontier, and the chain has nothing to add. The
	 * comparison is exact: the frontier holds a reference, so
	 * its address cannot be reused by another statement while
	 * it is set, and the hint tells apart the keys of a
	 * multikey statement, which backs one entry per key.
	 */
	if (vy_entry_is_equal(curr, builder->last))
		return;
	/* Consumed DELETEs go to vy_cache_builder_add_delete(). */
	assert(vy_stmt_type(curr.stmt) != IPROTO_DELETE);
	/*
	 * A prepared result is retractable and never admitted --
	 * see the prepared-data contract in vy_cache_insert().
	 */
	if (vy_lsn_is_prepared(vy_stmt_lsn(curr.stmt))) {
		vy_cache_builder_break_link(builder);
		return;
	}
	/*
	 * A stale result is shadowed by a version above the read
	 * view: it is not the latest and must not be cached, and
	 * the advance that produced it crossed the shadow, so no
	 * link may span the gap either (see
	 * vy_read_iterator::is_stale; VY_STMT_STALE carries the
	 * same verdict for a primary tuple resolved from a
	 * secondary key after the advance). The chain breaks,
	 * exactly as it does in vy_cache_builder_add_delete().
	 */
	if (is_stale || vy_stmt_has_flag(curr.stmt, VY_STMT_STALE)) {
		vy_cache_builder_break_link(builder);
		return;
	}
	/*
	 * A statement the cache refuses breaks the chain, which
	 * cannot continue through an uncached statement -- the
	 * next result starts a new one.
	 */
	if (vy_cache_insert(cache, curr, builder, builder->last) == NULL)
		vy_cache_builder_break_link(builder);
}

/** See the comment in vy_cache.h. */
void
vy_cache_builder_add_delete(struct vy_cache_builder *builder,
			    struct vy_entry delete_key,
			    int64_t delete_lsn, bool is_stale)
{
	struct vy_cache *cache = builder->cache;
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
#ifndef NDEBUG
	vy_linklog_event(builder, "DEL", delete_key, delete_lsn, is_stale);
#endif
	/*
	 * No link may span a retractable shadow (see vy_cache.h),
	 * and a stale verdict is shadowed by a version above the
	 * read view: the chain breaks. The cached entry, if any,
	 * is left alone -- the verdict is tentative (a prepared
	 * shadow may roll back, a stale reader's knowledge is
	 * outdated), and once the key is proven dead, the
	 * recorded DELETE replaces the stale row in place.
	 */
	if (vy_lsn_is_prepared(delete_lsn) || is_stale) {
		vy_cache_builder_break_link(builder);
		return;
	}
	/*
	 * A DELETE at or below the read view horizon serves no
	 * reader that can ever exist: nothing is recorded, and
	 * the chain is not advanced either -- the neighbors link
	 * up directly across the dead key. An unknown LSN, zero
	 * -- the shadowing write was compacted away entirely --
	 * lies below any horizon. The stale cached row, if any,
	 * is dropped here, because nothing else ever removes it.
	 * Invalidation never fired: a secondary index is not
	 * written when the row leaves it through a primary-only
	 * statement. The DELETE that would replace the row in
	 * place is refused right here. Eviction never picks the
	 * row either: every scan crossing the key yields it, and
	 * so re-heats it, before the primary resolution filters
	 * it from the results. This refusal is the only place
	 * that ever learns the cached row is dead.
	 */
	int64_t horizon = vy_tx_manager_horizon(cache->env->xm);
	if (delete_lsn <= horizon) {
#ifndef NDEBUG
		if (vy_linklog_is_enabled())
			fprintf(stderr, "REF cache=%p scan=%llu dlsn=%lld "
				"horizon=%lld key=%.60s\n", (void *)cache,
				(unsigned long long)builder->scan_id,
				(long long)delete_lsn, (long long)horizon,
				vy_stmt_str(delete_key.stmt));
#endif
		vy_cache_evict(cache, delete_key);
		/*
		 * The dropped row may be the chain's own frontier
		 * -- the scan was served the stale row before
		 * proving it dead. The chain breaks rather than
		 * completing a link from a removed entry.
		 */
		if (vy_entry_is_equal(builder->last, delete_key))
			vy_cache_builder_break_link(builder);
		return;
	}
	struct vy_entry prev = builder->last;
	assert(prev.stmt == NULL ||
	       !vy_lsn_is_prepared(vy_stmt_lsn(prev.stmt)));
	/*
	 * A DELETE fuses into the previous chain entry when that
	 * entry is a DELETE or a bound key: the new one is not
	 * cached at all, and the cached entry's LSN is raised to
	 * the newer of the two instead. E.g. after delete{2},
	 * delete{3}, delete{4} a scan leaves one entry, key {2}
	 * at the LSN of delete{4}, linked to its neighbors; a
	 * GT{1} scan over the same deletes leaves only its own
	 * start bound, raised the same way. A reader at or
	 * above that LSN sees all three keys as simply absent. A
	 * reader below it does not observe the entry's links
	 * (see the landing rule in vy_cache_iterator_seek()) and
	 * descends to the deeper sources, where each DELETE
	 * still lives. Even a reader that could see delete{2} in
	 * the cache pays the descent: the price of one LSN for
	 * all three keys. The raise is safe. The statement is
	 * the cache's private copy, and the LSN takes no part in
	 * the tree order. A higher LSN only narrows the entry's
	 * visibility: sending more readers to the deeper sources
	 * never serves stale data.
	 */
	if (prev.stmt != NULL && vy_stmt_is_cache_meta(prev.stmt)) {
		if (vy_stmt_lsn(prev.stmt) < delete_lsn)
			vy_stmt_set_lsn(prev.stmt, delete_lsn);
		return;
	}
	struct vy_entry delete_entry;
	delete_entry.stmt = vy_stmt_dup(delete_key.stmt);
	if (delete_entry.stmt == NULL) {
		vy_cache_builder_break_link(builder);
		return;
	}
	delete_entry.hint = delete_key.hint;
	vy_stmt_set_type(delete_entry.stmt, IPROTO_DELETE);
	vy_stmt_set_lsn(delete_entry.stmt, delete_lsn);
	if (vy_cache_insert(cache, delete_entry, builder, prev) == NULL)
		vy_cache_builder_break_link(builder);
	tuple_unref(delete_entry.stmt);
}

/**
 * The frontier's validated tree position. The builder remembers
 * where the frontier sits in the tree (see
 * vy_cache_builder::last_pos), but the tree reshapes under
 * concurrent admissions and evictions, so the position is
 * trusted only after revalidation: the element it addresses
 * must still hold the frontier's very statement.
 * @param builder the builder.
 * @param tree the cache tree.
 * @retval the frontier's element, at builder->last_pos.
 * @retval NULL no frontier, or its position went stale.
 */
static struct vy_cache_entry *
vy_cache_builder_get_last(struct vy_cache_builder *builder,
			  struct vy_cache_tree *tree)
{
	if (builder->last.stmt == NULL)
		return NULL;
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(tree, &builder->last_pos);
	if (node == NULL || !vy_entry_is_equal(node->entry, builder->last))
		return NULL;
	return node;
}

/** See the comment in vy_cache.h. */
void
vy_cache_builder_on_read(struct vy_cache_builder *builder,
			 struct vy_cache_entry *node,
			 struct vy_cache_tree_iterator pos)
{
	struct vy_cache *cache = builder->cache;
	/* The cache is disabled. */
	if (cache->env->mem_quota == 0)
		return;
	/*
	 * A result served over observed links consulted no deeper
	 * source in the gap it rode over, and the chain proves the
	 * gap empty at every version: nothing above any read view
	 * can have been skipped, so the links extend for every
	 * reader.
	 */
	/*
	 * Open the pending link in place: the entry is at hand --
	 * under the cache iterator's cursor, or just inserted --
	 * and the reader has not yielded since, so no lookup is
	 * needed. A write after this point aborts the pending
	 * link as usual; anything committed before it is observed by the
	 * descent that eventually consumes it, since the descent
	 * starts after this call. No version bump: readers do not
	 * observe pending links.
	 */
	node->pending_link_direction = iterator_direction(builder->order);
	node->scan_id = builder->scan_id;
	/*
	 * The entry counts as a result the reader produced onto
	 * the chain. Otherwise a scan served to its end over a
	 * chain whose end bound was evicted would close as
	 * resultless and never re-insert the bound. A bound key
	 * does not count: it may carry a fused DELETE's LSN and
	 * be the frontier, but it is not a recorded statement.
	 */
	if (!vy_stmt_is_bound(node->entry.stmt))
		builder->chain_length++;
	/*
	 * Refresh the position even when the frontier statement
	 * is unchanged: the entry may have moved in the tree.
	 */
	builder->last_pos = pos;
	if (vy_entry_is_equal(builder->last, node->entry))
		return;
	/*
	 * Advance the frontier, remembering the cached statement
	 * -- not the one the read produced, the cache may have
	 * kept its own copy of the same row -- so the next added
	 * entry looks its predecessor up by pointer before
	 * falling back to a key comparison.
	 */
	tuple_ref(node->entry.stmt);
	if (builder->last.stmt != NULL)
		tuple_unref(builder->last.stmt);
	builder->last = node->entry;
}

/** See the comment in vy_cache.h. */
void
vy_cache_builder_break_link(struct vy_cache_builder *builder)
{
	struct vy_cache *cache = builder->cache;
	struct vy_entry last = builder->last;
	if (last.stmt == NULL)
		return;
	builder->last = vy_entry_none();
	builder->last_pos = vy_cache_tree_invalid_iterator();
	if (!vy_stmt_is_bound(last.stmt)) {
		tuple_unref(last.stmt);
		return;
	}
	/*
	 * The chain is dropped while its start bound is still the
	 * frontier: the scan recorded nothing. An empty range is
	 * not worth remembering -- people search the data they
	 * store, and a repeat miss is served by the deeper
	 * sources, whose bloom filters cover key prefixes. Every
	 * way to drop a chain funnels here: the close of a
	 * resultless scan, a reader sent to a read view
	 * mid-scan, a prepared skip, and plain abandonment. The
	 * start bound is removed only when it is this chain's
	 * alone:
	 * the slot still carries this chain's scan id and
	 * no link is attached on either side. A re-stamped or
	 * aborted start bound is left alone: another chain or the
	 * eviction hand owns it now.
	 */
	bool exact;
	struct vy_cache_tree_iterator pos =
		vy_cache_tree_lower_bound(cache->tree, last, &exact);
	tuple_unref(last.stmt);
	if (!exact)
		return;
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(cache->tree, &pos);
	if (node->scan_id != builder->scan_id)
		return;
	assert(node->pending_link_direction ==
	       iterator_direction(builder->order));
	if (node->is_linked)
		return;
	struct vy_cache_tree_iterator prev_pos = pos;
	struct vy_cache_entry *prev =
		vy_cache_tree_step(cache->tree, &prev_pos, -1);
	if (prev != NULL && prev->is_linked)
		return;
	/*
	 * The pending links facing the bound survive its removal.
	 * A pending link is aborted by any write into the range
	 * it is extended over -- the abortion reaches across
	 * chain metadata, this bound included, see
	 * vy_cache_clear_pending_links() -- so a link still
	 * pending here has seen no write since it was opened, and
	 * its opener's walk observed everything it will claim,
	 * this bound's position included. Removing the bound
	 * changes adjacency, not the claim.
	 */
	vy_cache_evict_pos(cache, pos);
}

/* }}} vy_cache_builder */

/* {{{ vy_cache_iterator */

/**
 * Check if the iterator has ever searched the cache: a search
 * stamps the cache version, and version zero predates every
 * cache.
 */
static bool
vy_cache_iterator_is_started(struct vy_cache_iterator *itr)
{
	return itr->version != 0;
}

/**
 * Replace the iterator's current statement, moving the held
 * reference from the old one to the new.
 */
static void
vy_cache_iterator_set_curr(struct vy_cache_iterator *itr,
			   struct vy_entry entry)
{
	if (entry.stmt != NULL)
		tuple_ref(entry.stmt);
	if (itr->curr.stmt != NULL)
		tuple_unref(itr->curr.stmt);
	itr->curr = entry;
}

/**
 * Check if an entry ends an EQ scan: it lies beyond the last
 * possible match. A non-matching entry does; so does a bound
 * key of the searched key (or a shorter one) on the scan's far
 * side, which sorts after every match while still comparing
 * equal to the search key. A bound ending the scan is touched:
 * the stop is a use, whether reached by a hop or landed on.
 * @param itr the iterator.
 * @param node the entry to check.
 * @retval true the entry ends the scan.
 * @retval false the scan continues.
 */
static inline bool
vy_cache_iterator_is_eq_end(struct vy_cache_iterator *itr,
			    struct vy_cache_entry *node)
{
	if (itr->iterator_type != ITER_EQ && itr->iterator_type != ITER_REQ)
		return false;
	struct key_def *cmp_def = itr->cache->cmp_def;
	struct tuple *stmt = node->entry.stmt;
	/* A matching entry, unless it is the key's far-side bound. */
	if (vy_entry_compare(itr->key, node->entry, cmp_def) == 0 &&
	    (vy_bound_sign(stmt) != iterator_direction(itr->iterator_type) ||
	     vy_stmt_key_part_count(stmt, cmp_def) >
	     vy_stmt_key_part_count(itr->key.stmt, cmp_def)))
		return false;
	/* The stop is a use. */
	if (vy_stmt_is_bound(stmt))
		vy_cache_entry_touch(node, itr->cache->is_primary);
	return true;
}

/**
 * Advance itr->curr_pos to the next tuple visible in the
 * iterator's read view, hopping bound keys and consuming
 * DELETEs. A visible DELETE is its key's newest version --
 * absence -- and is never yielded: its links carry the chain
 * across it. On return itr->curr is the next tuple (referenced)
 * or none at the end of matches.
 * @param itr the iterator.
 * @param[in,out] is_linked accumulates whether every link
 * crossed so far -- starting from the caller's position -- was
 * observed: it only ever goes from true to false. An entry
 * invisible in the read view clears it: the reader's version of
 * its key is in the deeper sources.
 */
static void
vy_cache_iterator_hop(struct vy_cache_iterator *itr, bool *is_linked)
{
	/* An advance leaves the old statement behind. */
	vy_cache_iterator_set_curr(itr, vy_entry_none());
	struct vy_cache_tree *tree = itr->cache->tree;
	struct vy_tx_manager *xm = itr->cache->env->xm;
	int64_t vlsn = (*itr->read_view)->vlsn;
	int dir = iterator_direction(itr->iterator_type);
	/*
	 * The entry being left by the step. Missing when the walk
	 * starts before the first entry: the caller then seeded
	 * *is_linked false, and the crossing below short-circuits
	 * without reading the missing entry's link.
	 */
	struct vy_cache_entry *prev =
		vy_cache_tree_iterator_get_elem(tree, &itr->curr_pos);
	while (true) {
		struct vy_cache_entry *next =
			vy_cache_tree_step(tree, &itr->curr_pos, dir);
		/*
		 * Running off either end of the tree crosses a
		 * range no link covers.
		 */
		if (next == NULL) {
			*is_linked = false;
			return;
		}
		/*
		 * Cross the link between the current position and
		 * the next entry. Its home is the smaller of the
		 * two: going up, the entry being left; going down,
		 * the entry arrived at.
		 */
		struct tuple *stmt = next->entry.stmt;
		bool visible = vy_stmt_lsn(stmt) <= vlsn &&
			!vy_stmt_maybe_stale(stmt, itr->cache->is_primary);
		/*
		 * An invisible entry of any kind clears
		 * *is_linked: the reader's version of its key is
		 * in the deeper sources, and a DELETE may hide
		 * fused keys the reader still sees alive. A
		 * maybe-stale entry is invisible to every reader:
		 * the deeper sources hold the truth. The hand
		 * collects it.
		 */
		*is_linked = *is_linked && visible &&
			     (dir > 0 ? prev : next)->is_linked;
		prev = next;
		/*
		 * The end of matches ends the walk. Checked ahead
		 * of the common case: a tuple past the searched
		 * range must not be served. One comparison for a
		 * non-EQ scan.
		 */
		if (vy_cache_iterator_is_eq_end(itr, next))
			return;
		/* The common case: a visible tuple is the result. */
		if (visible && !vy_stmt_is_cache_meta(stmt)) {
			vy_cache_iterator_set_curr(itr, next->entry);
			return;
		}
		/*
		 * An empty-key bound reached by a step can only
		 * be the end of the key space: nothing sorts
		 * beyond it on the arrival side. The reader has
		 * exhausted the tuples.
		 */
		if (vy_stmt_is_empty_key(stmt)) {
			vy_cache_entry_touch(next, itr->cache->is_primary);
			return;
		}
		/*
		 * A crossed bound key or DELETE earns heat only
		 * while some read view can still see under its
		 * LSN: kept hot, it cannot be evicted while a
		 * reader may still need it, which would sever
		 * the chain. Once expired -- or plain, at LSN
		 * zero -- it goes cold and is evicted by fusion,
		 * which keeps the chain whole. Bound keys earn
		 * heat from being landed on, granting a stop, or
		 * ending a scan -- not from being crossed.
		 */
		if (vy_stmt_is_cache_meta(stmt) &&
		    vy_stmt_is_above_horizon(stmt, xm))
			vy_cache_entry_touch(next, itr->cache->is_primary);
	}
}

/**
 * Position the iterator to the first cache entry satisfying the
 * search criteria and following @a last.
 * @param itr the iterator.
 * @param last the key to seek past, none to start from the
 * search key.
 * @param[out] is_linked set if the landing consumed only
 * observed links and so no statement can precede it in the
 * deeper sources.
 */
static void
vy_cache_iterator_seek(struct vy_cache_iterator *itr, struct vy_entry last,
		       bool *is_linked)
{
	struct vy_cache_tree *tree = itr->cache->tree;
	int dir = iterator_direction(itr->iterator_type);
	int64_t vlsn = (*itr->read_view)->vlsn;

	vy_cache_iterator_set_curr(itr, vy_entry_none());
	itr->cache->stat.lookup++;

	/*
	 * Position at the search key. A resume is a plain search:
	 * upper_bound to restart past the last returned tuple,
	 * lower_bound to restart before it. A first seek positions
	 * at the side of the key's match range the scan starts
	 * from -- a side a shared, unmarked key cannot express --
	 * so it goes through the scan's start bound, made at the
	 * builder's creation. Usually no search is needed: the
	 * bound was inserted at the builder's creation and is
	 * still the frontier -- nothing has been produced yet --
	 * so its position is at hand. The landing is taken only
	 * when the frontier is that bound, unfused -- a start
	 * bound fused into an entry below the scan's start must
	 * not become a landing, or the walk would serve keys
	 * before the start -- and its position revalidates.
	 *
	 * The landing seeds the walk's trust. E.g. a GE{20} scan
	 * of a cache holding {10} and {30}: landing on {30} skips
	 * the range [20, 30), and only a {10} -> {30} link can
	 * prove the skipped range empty -- without it, a deeper
	 * source may still hold a key 25, so the walk enters
	 * untrusted. An exact landing -- on the scan's own [20]-
	 * bound -- skips nothing: the range begins at the very
	 * entry the iterator stands on, and there is no gap a
	 * link would have to prove empty.
	 */
	struct vy_entry pos_key = last.stmt != NULL ?
		last : itr->builder->start_bound;
	if (pos_key.stmt == NULL) {
		/*
		 * The start bound is missing only if its
		 * allocation failed at the builder's creation.
		 * Unable to position, the cache serves nothing
		 * for this scan: an empty result is always
		 * correct.
		 */
		itr->curr_pos = vy_cache_tree_invalid_iterator();
		*is_linked = false;
		return;
	}
	struct vy_cache_builder *builder = itr->builder;
	bool at_start = false;
	if (last.stmt == NULL) {
		struct vy_entry front = builder->last;
		struct vy_entry bound = builder->start_bound;
		if (front.stmt != NULL &&
		    (vy_entry_is_equal(front, bound) ||
		     vy_entry_is_bound_of(front, bound,
					  vy_bound_sign(bound.stmt),
					  itr->cache->cmp_def)) &&
		    vy_cache_builder_get_last(builder, tree) != NULL) {
			itr->curr_pos = builder->last_pos;
			at_start = true;
		}
	}
	if (!at_start)
		itr->curr_pos = dir > 0 && last.stmt != NULL ?
			vy_cache_tree_upper_bound(tree, pos_key, NULL) :
			vy_cache_tree_lower_bound(tree, pos_key, &at_start);
	if (dir > 0) {
		/*
		 * Landing on the resume position's own bound is
		 * exact: a resume position is a full key, so
		 * nothing sorts between it and its bound -- nothing
		 * was moved over.
		 */
		struct vy_cache_entry *node =
			vy_cache_tree_iterator_get_elem(tree,
							&itr->curr_pos);
		if (node != NULL && last.stmt != NULL &&
		    vy_entry_is_bound_of(node->entry, last, dir,
					 itr->cache->cmp_def))
			at_start = true;
		/*
		 * The walk starts at the entry before the scan's
		 * first candidate -- the landing itself when the
		 * scan starts exactly there, its predecessor
		 * otherwise -- and the hop classifies the landing
		 * as its first arrival, checking the crossing
		 * into it.
		 *
		 * The landing rule: the start entry decides the
		 * initial *is_linked. The links around a DELETE --
		 * possibly fused into a bound key -- are used only
		 * by a reader that sees it: a reader below its LSN
		 * may still see a key fused into it alive and must
		 * descend for it. A tuple is a witness for any
		 * reader: its gaps held no committed key of any
		 * version when the link formed. A missing start
		 * entry witnesses nothing: the walk enters over an
		 * unclaimed range.
		 */
		if (!at_start)
			vy_cache_tree_iterator_prev(tree, &itr->curr_pos);
		struct vy_cache_entry *start =
			vy_cache_tree_iterator_get_elem(tree,
							&itr->curr_pos);
		*is_linked = start != NULL &&
			(!vy_stmt_is_cache_meta(start->entry.stmt) ||
			 vy_stmt_lsn(start->entry.stmt) <= vlsn) &&
			!vy_stmt_maybe_stale(start->entry.stmt,
					     itr->cache->is_primary);
		/* Chain use is use: a witnessing bound stays hot. */
		if (*is_linked && vy_stmt_is_bound(start->entry.stmt))
			vy_cache_entry_touch(start,
					     itr->cache->is_primary);
		vy_cache_iterator_hop(itr, is_linked);
	} else {
		struct vy_cache_entry *node =
			vy_cache_tree_iterator_get_elem(tree,
							&itr->curr_pos);
		if (node == NULL) {
			/*
			 * No landing: the positioning key is
			 * beyond every entry. The candidates lie
			 * below, and the hop starts at the last
			 * entry, reached over a tail range no
			 * link witnesses.
			 */
			*is_linked = false;
			vy_cache_iterator_hop(itr, is_linked);
			return;
		}
		/*
		 * Reverse: the candidates lie before the landing,
		 * and the first crossing -- from the landing down
		 * to its predecessor -- is checked by the hop on
		 * arrival, covering the reverse scan's start: the
		 * predecessor necessarily precedes the positioning
		 * key. The landing entry is stepped away from, never
		 * classified, so its visibility must witness the
		 * range it rules: a DELETE guards its fused keys
		 * by being invisible to any reader that could see
		 * one of them alive.
		 *
		 * A resume lands on the previous page's last tuple,
		 * with the resumed scan's own start bound one step
		 * below it. The range between them is empty:
		 * nothing sorts between a full key and its infimum,
		 * so the crossing into the start bound needs no
		 * link to witness it (the forward direction reaches
		 * the same conclusion via at_start). Step onto the
		 * start bound and hop from there. The bound entry
		 * is shared with any earlier scan's bound at the
		 * same position, and DELETEs fused into it raise
		 * its LSN: a page resumed at {40} lands on the
		 * bound an earlier page left there, delete{20} at
		 * LSN 2000 fused into it. A link is used only by
		 * readers that see the entry it touches: to a
		 * reader at read view 1500 the crossing into the
		 * bound is not covered, and it descends -- {20}
		 * is still alive for it.
		 */
		struct key_def *cmp_def = itr->cache->cmp_def;
		if (last.stmt != NULL &&
		    vy_entry_compare(node->entry, last, cmp_def) == 0) {
			struct vy_cache_tree_iterator below = itr->curr_pos;
			struct vy_cache_entry *bound =
				vy_cache_tree_step(tree, &below, -1);
			if (bound != NULL &&
			    vy_entry_is_bound_of(bound->entry, last, dir,
						 cmp_def)) {
				itr->curr_pos = below;
				*is_linked =
					vy_stmt_lsn(bound->entry.stmt) <= vlsn;
				vy_cache_iterator_hop(itr, is_linked);
				return;
			}
		}
		*is_linked = vy_stmt_lsn(node->entry.stmt) <= vlsn &&
			!vy_stmt_maybe_stale(node->entry.stmt,
					     itr->cache->is_primary);
		vy_cache_iterator_hop(itr, is_linked);
	}
}

/**
 * Yield the tuple the iterator stands on: heat its entry (a
 * GCLOCK use), account the cache hit, report the entry to the
 * reader's chain when it was reached over observed links, and
 * append the tuple to the output history.
 * @param itr the iterator.
 * @param[out] history the output history.
 * @param is_linked the entry was reached over observed links.
 * @retval 0 success.
 * @retval -1 memory allocation error.
 */
static int
vy_cache_iterator_yield(struct vy_cache_iterator *itr,
			struct vy_history *history, bool is_linked)
{
	assert(itr->curr.stmt != NULL);
	struct vy_cache_entry *node =
		vy_cache_tree_iterator_get_elem(itr->cache->tree,
						&itr->curr_pos);
	vy_cache_entry_touch(node, itr->cache->is_primary);
	if (is_linked)
		vy_cache_builder_on_read(itr->builder, node, itr->curr_pos);
	vy_stmt_counter_acct_tuple(&itr->cache->stat.get, itr->curr.stmt);
	return vy_history_append_stmt(history, itr->curr);
}

NODISCARD int
vy_cache_iterator_next(struct vy_cache_iterator *itr,
		       struct vy_history *history, bool *stop)
{
	vy_history_cleanup(history);

	if (!vy_cache_iterator_is_started(itr)) {
		assert(itr->curr.stmt == NULL);
		itr->version = itr->cache->version;
		vy_cache_iterator_seek(itr, vy_entry_none(), stop);
	} else {
		assert(itr->version == itr->cache->version);
		if (itr->curr.stmt == NULL)
			return 0;
		*stop = true;
		vy_cache_iterator_hop(itr, stop);
	}

	if (itr->curr.stmt == NULL)
		return 0;
	return vy_cache_iterator_yield(itr, history, *stop);
}

NODISCARD int
vy_cache_iterator_skip(struct vy_cache_iterator *itr, struct vy_entry last,
		       struct vy_history *history, bool *stop)
{
	assert(!vy_cache_iterator_is_started(itr) ||
	       itr->version == itr->cache->version);

	/*
	 * Check if the iterator is already positioned
	 * at the statement following last.
	 */
	if (vy_cache_iterator_is_started(itr) &&
	    (itr->curr.stmt == NULL || last.stmt == NULL ||
	     iterator_direction(itr->iterator_type) *
	     vy_entry_compare(itr->curr, last, itr->cache->cmp_def) > 0))
		return 0;

	vy_history_cleanup(history);

	itr->version = itr->cache->version;
	vy_cache_iterator_seek(itr, last, stop);

	if (itr->curr.stmt == NULL)
		return 0;
	return vy_cache_iterator_yield(itr, history, *stop);
}

NODISCARD int
vy_cache_iterator_restore(struct vy_cache_iterator *itr, struct vy_entry last,
			  struct vy_history *history, bool *stop)
{
	if (!vy_cache_iterator_is_started(itr) ||
	    itr->version == itr->cache->version)
		return 0;

	itr->version = itr->cache->version;
	/*
	 * The chain's frontier is the reader's own position: the
	 * builder's last entry holds the statement the reader
	 * last returned, and its tree position is refreshed on
	 * every frontier advance. When the frontier is the
	 * reader's own position and revalidates, the restore is
	 * a hop from it -- no tree descent -- re-examining the
	 * range between the reader and its next candidate
	 * against the new content. A reader whose chain is dead
	 * -- broken, refused, or never built -- re-seeks
	 * instead. The re-seek is cheap in context: such a
	 * reader is mid-descent into the deeper sources, whose
	 * cost dwarfs the seek, or on a rare path.
	 */
	struct vy_cache_builder *builder = itr->builder;
	struct vy_cache_entry *front = NULL;
	if (last.stmt != NULL && vy_entry_is_equal(builder->last, last))
		front = vy_cache_builder_get_last(builder,
						  itr->cache->tree);
	struct vy_entry old = itr->curr;
	if (old.stmt != NULL)
		tuple_ref(old.stmt);
	if (front != NULL) {
		itr->curr_pos = builder->last_pos;
		/* See the landing rule in vy_cache_iterator_seek(). */
		*stop = (!vy_stmt_is_cache_meta(front->entry.stmt) ||
			 vy_stmt_lsn(front->entry.stmt) <=
			 (*itr->read_view)->vlsn) &&
			!vy_stmt_maybe_stale(front->entry.stmt,
					     itr->cache->is_primary);
		vy_cache_iterator_hop(itr, stop);
	} else {
		vy_cache_iterator_seek(itr, last, stop);
	}
	bool changed = !vy_entry_is_equal(itr->curr, old);
	if (old.stmt != NULL)
		tuple_unref(old.stmt);
	if (!changed)
		return 0;

	vy_history_cleanup(history);
	if (itr->curr.stmt != NULL &&
	    vy_cache_iterator_yield(itr, history, *stop) != 0)
		return -1;
	return 1;
}

void
vy_cache_iterator_close(struct vy_cache_iterator *itr)
{
	vy_cache_iterator_set_curr(itr, vy_entry_none());
	TRASH(itr);
}

void
vy_cache_iterator_open(struct vy_cache_iterator *itr, struct vy_cache *cache,
		       enum iterator_type iterator_type, struct vy_entry key,
		       const struct vy_read_view **rv,
		       struct vy_cache_builder *builder)
{
	assert(builder != NULL);
	itr->cache = cache;
	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->read_view = rv;
	itr->builder = builder;

	itr->curr = vy_entry_none();
	itr->curr_pos = vy_cache_tree_invalid_iterator();

	itr->version = 0;
}

/* }}} vy_cache_iterator */
