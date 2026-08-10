#ifndef INCLUDES_TARANTOOL_BOX_VY_CACHE_H
#define INCLUDES_TARANTOOL_BOX_VY_CACHE_H
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

#include <stdint.h>
#include <stdbool.h>

#include <small/rlist.h>

#include "iterator_type.h"
#include "vy_stmt.h" /* for comparators */
#include "vy_read_view.h"
#include "vy_stat.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_history;
struct vy_tx_manager;

/* {{{ vy_cache_entry */

/**
 * The tuple cache is an ordered tree of cache entries. An entry
 * holds one statement. It is either an INSERT or REPLACE
 * previously returned to a reader, or a DELETE, or a bound key
 * -- an infimum or supremum bound of the tuples matching a key
 * (VY_STMT_INFIMUM/VY_STMT_SUPREMUM). A DELETE entry states
 * that the key's latest version is a deletion, while mems or
 * runs may hold older versions visible to some older read view:
 * the cache stores a single, latest version of data plus the
 * fact that older versions exist elsewhere. Only INSERTs and
 * REPLACEs are ever returned to readers. During a scan, when
 * the cache iterator encounters a DELETE, it checks the read
 * view LSN against the DELETE LSN: a read view with a newer
 * LSN skips the DELETE entry and proceeds to the next tuple; a
 * read view with an older LSN must descend to mems or runs for
 * the matching version of tuples.
 *
 * The cache is a mirror of the committed data. Prepared, but not
 * committed data is never admitted, and this is correct because
 * cache invalidation happens already at txn prepare: from that
 * moment until a reader re-populates the entry, the cache has a
 * gap where the write landed, never a stale entry. A gap sends
 * the reader to the deeper sources, which do serve prepared data
 * to the readers that may see it.
 *
 * Each index, primary or secondary, has a cache of its own,
 * storing full tuples. The tuples are referenced, so most of
 * the time a secondary index cache shares the tuple object with
 * the primary. Each cache entry has an own heat, and the shared
 * tuple has a heat of its own -- the row's temperature: a read
 * through any index heats both. A secondary index evicts its
 * entries by their own heat, so a tuple cold in one index leaves
 * it however hot the row is elsewhere. The primary index is the
 * row's home and follows the shared heat, which the primary
 * eviction hand alone cools: the row outlives its use through
 * any index, not just the primary's. The cooling is weighted by
 * the row's size: at equal heat a smaller row lives longer --
 * the quota is bytes, and evicting one large row frees room for
 * many small ones. A secondary cache admits a
 * tuple only while the row is resident in the primary cache --
 * the primary entry is the write path's rendezvous with the row
 * -- and a secondary entry whose row leaves it, or is
 * overwritten, is maybe stale: invisible to every reader,
 * collected on encounter. A tuple that is not maybe stale is
 * therefore the newest committed version of its row, and a
 * secondary read serves it as is; any other secondary result
 * is resolved against the primary in
 * vy_get_by_secondary_tuple().
 *
 * Two adjacent entries can be connected with a link, meaning no
 * tuples exist between them. A fully cached scan is a chain of
 * linked entries bounded by keys. For example, an EQ scan of key
 * {10} in a non-unique index holding tuples {10, 1} and {10, 2}
 * leaves the footprint
 *
 *   [10]- -> {10, 1} -> {10, 2} -> [10]+
 *
 * where [10]- and [10]+ are the infimum and supremum keys of 10
 * and each arrow is a link. The bound keys complete the chain at
 * both ends: the infimum proves no match precedes {10, 1}, the
 * supremum proves none follows {10, 2}, so a repeated scan is
 * served without leaving the cache.
 *
 * A DELETE does not always become an entry of its own. When the
 * chain being built already ends with a bound key or a DELETE,
 * a newly read DELETE is fused into that entry: no new entry is
 * created, and the existing entry's LSN is raised to the fused
 * DELETE's LSN, remembering the newest of them. E.g. a full
 * scan of a table holding tuples {1} and {5} and DELETEs 2, 3,
 * and 4 leaves the following trace in the cache:
 *
 *   {1} -> D{2} -> {5}
 *
 * with a single DELETE entry carrying the LSN of delete{4}; a
 * GE{2} scan over the same table records all three deletes on
 * its own start bound instead, [2]- at that LSN, and no DELETE
 * entry appears at all. The same happens on insertion: a DELETE
 * that would land right next to a bound key or a DELETE left by
 * another scan is fused into it. The one LSN serves every fused
 * key. A reader at or above it sees the entry and follows its
 * links: each fused key is simply absent. A reader below it
 * does not see the entry and does not follow its links; it
 * descends to the deeper sources, where each DELETE still
 * lives at its own LSN.
 *
 * A pending link is a link under construction: opened before the
 * reader yields, completed when the reader produces the entry on
 * the other side, destroyed by any write between the two entries.
 * Each read iterator has an own unique id it uses to mark
 * its pending links. An entry holds at most one pending link. If
 * two read iterators try to extend a link from the same entry
 * concurrently, the one who came last wins.
 *
 * Glossary:
 *
 * - entry: an element of the cache tree -- a statement plus one
 *   word of link state.
 * - bound key: an entry holding a key marked VY_STMT_INFIMUM or
 *   VY_STMT_SUPREMUM; carries links, never returned to readers.
 * - chain: the linked entries of one scan, ended by bound keys.
 * - start bound, end bound: the bound keys a scan inserts at
 *   the two ends of its chain.
 * - pending link: a link under construction: opened before the
 *   reader yields, completed by the next result, aborted by a
 *   write into its range or by eviction of a neighbor.
 * - scan id: the unique id of one range scan, stamped on the
 *   pending links it opens and checked at their completion.
 * - frontier: the chain's last added point.
 * - hop: the cache iterator's advance to the next visible
 *   tuple, over bound keys and consumed DELETEs.
 * - heat: the GCLOCK use counter; a use raises it, the eviction
 *   hand cools it, zero means evict. Meta entries and secondary
 *   tuple entries go by their entry's own heat; a primary tuple
 *   entry goes by the shared statement heat, cooled by the
 *   row's size in units of the resident mean -- see
 *   VY_STMT_HEAT_MASK -- and leaves its entry heat untouched.
 * - maybe stale: a secondary tuple entry whose statement may
 *   not be the row's latest: it is no longer cached in the
 *   primary, where a write would find -- and retire -- it;
 *   invisible to every reader, collected on encounter by a
 *   point lookup or the eviction hand.
 * - to fuse: to absorb a DELETE or a bound key into an adjacent
 *   entry, raising that entry's LSN, instead of keeping two
 *   entries; eviction likewise fuses two links into one over a
 *   removed entry.
 * - guarded: carrying a DELETE LSN above the oldest read view:
 *   some reader may still see a deleted key alive under it.
 * - to land, to cross: a seek lands on an entry; the walk then
 *   crosses the gaps between entries. A bound key landed on or
 *   stopped at earns heat; one merely crossed does not.
 */
enum {
	/** Width of vy_cache_entry::scan_id. */
	VY_CACHE_SCAN_ID_BITS = 56,
	/** The heat cap of vy_cache_entry::heat. */
	VY_CACHE_HEAT_MAX = 3,
};

/** The largest scan id; the counter wraps past it. */
static const uint64_t VY_CACHE_SCAN_ID_MAX =
	UINT64_MAX >> (64 - VY_CACHE_SCAN_ID_BITS);

/** A cache tree element: a cached statement and its link state. */
struct vy_cache_entry {
	/** The cached statement: a tuple, a DELETE, or a bound key. */
	struct vy_entry entry;
	union {
		struct {
			/*
			 * No tuples exist between this entry and
			 * its successor; the largest entry never
			 * carries the flag. A forward scan reads
			 * the flag off the entry it leaves; a
			 * reverse scan first steps to the
			 * predecessor and reads the flag there to
			 * learn whether the crossing is covered.
			 * A link is used only by readers that
			 * see the entry it touches. A reader
			 * below the entry's LSN may still see a
			 * key fused into it alive: for that
			 * reader the links around the entry mean
			 * nothing, and it descends to mems and
			 * runs. This applies to any entry with a
			 * DELETE LSN -- a DELETE entry or a bound
			 * key a DELETE was fused into; a plain
			 * bound key, at LSN zero, is visible to
			 * every reader.
			 */
			uint64_t is_linked : 1;
			/*
			 * The direction a pending link is being
			 * extended from this entry -- the opening
			 * reader's iterator_direction() -- or 0
			 * when there is none. A pending link is
			 * opened at the reader's last result;
			 * its other end is not an entry yet -- it
			 * is wherever the next result lands. A
			 * reverse scan of key 10 over {10, 1} and
			 * {10, 2} returns {10, 2} first and opens
			 * a pending link at {10, 2} extending
			 * leftward; producing {10, 1} completes
			 * it into the {10, 1} -> {10, 2} link.
			 * A write into the extended range or the
			 * eviction of a neighbor aborts the
			 * pending link. The direction makes the
			 * abort selective: a write kills only
			 * the pending links extended over it --
			 * the predecessor's rightward one and the
			 * successor's leftward one. A pending
			 * link extended away from the write
			 * survives. The abort zeroes the id
			 * only; the direction is not cleared and
			 * may be stale.
			 * An entry holds at most one pending
			 * link: a new open replaces the previous
			 * one, whose completion then fails the
			 * id check.
			 */
			int64_t pending_link_direction : 2;
			/*
			 * The GCLOCK heat: the admission credit
			 * grants one surviving visit, every use
			 * raises it up to VY_CACHE_HEAT_MAX, the
			 * eviction hand decrements it, zero means
			 * evict. Unused by a primary tuple entry,
			 * which goes by the shared statement heat.
			 * Grades retention: a one-shot scan's
			 * entries live one cycle, the working set
			 * saturates and outlives them.
			 */
			uint64_t heat : 2;
			/* Free flag space. */
			uint64_t unused : 3;
			/*
			 * Id of the last range scan that read or
			 * inserted this entry. Each range scan is
			 * assigned a unique id, see
			 * vy_cache_builder::scan_id. The id
			 * is issued when the scan starts. Ids
			 * come from a single counter shared by
			 * all caches: if each entry counted on
			 * its own, an entry evicted and
			 * re-created while its scan sleeps would
			 * restart the count and could repeat an
			 * id the scan still remembers. Each
			 * new entry produced by this scan, or an
			 * existing entry returned by the scan, is
			 * then stamped with this id. The
			 * stamp is done before the read iterator
			 * descends to mems or runs (and yields
			 * for that), and acts as an intention
			 * lock on the link-to-be. When the scan
			 * obtains the next tuple, provided the
			 * scan_id is unchanged, it connects
			 * the two entries, allowing the next scan
			 * to be fully served from the cache.
			 * Meanwhile, a concurrent scan can
			 * re-stamp this entry, or a concurrent
			 * insert or delete may zero it, thus
			 * invalidating the intention lock. This
			 * arrangement guarantees that established
			 * links are consistent. It also isolates
			 * the cache consistency protocol from
			 * transaction consistency: the cache
			 * stays consistent regardless of the
			 * transaction isolation level or read
			 * view used by the scan. The id range is
			 * large enough to protect from a
			 * deliberate wrap-around attack: an idle
			 * cursor is a user-controlled object and
			 * can stay alive long enough to
			 * potentially produce an id collision
			 * under heavy load. There is no protocol
			 * for clearing stale ids and nothing
			 * relies on it: a scan can be abandoned,
			 * in which case its entry's scan_id
			 * stays with it until it is evicted. The
			 * same scan_id is used to establish
			 * leftward and rightward links: the last
			 * writer wins, and the link lost in the
			 * conflict is established by a later
			 * repeat scan.
			 */
			uint64_t scan_id :
				VY_CACHE_SCAN_ID_BITS;
		};
		/* The whole word, for init and transplant. */
		uint64_t bits;
	};
};

static_assert(sizeof(struct vy_cache_entry) == 24,
	      "the entry is stored inline, keep it lean");

#define VY_CACHE_TREE_EXTENT_SIZE (16 * 1024)

#define BPS_TREE_NAME vy_cache_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_EXTENT_SIZE VY_CACHE_TREE_EXTENT_SIZE
/*
 * The tree order is the total key order of vy_bound_cmp(). The
 * search key type is the element type, vy_entry, and the
 * comparator takes a key's position among equal keys from the
 * VY_STMT_INFIMUM/VY_STMT_SUPREMUM flags of the key statement
 * itself. To search for a bound key's position, search with a
 * statement carrying the same flag -- this is why the builder
 * makes marked copies (see vy_cache_builder::start_bound); a
 * statement with neither flag searches as a bare key.
 */
#define BPS_TREE_COMPARE(a, b, cmp_def) \
	vy_bound_cmp((a).entry, (b).entry, cmp_def)
#define BPS_TREE_COMPARE_KEY(a, b, cmp_def) vy_bound_cmp((a).entry, b, cmp_def)
#define bps_tree_elem_t struct vy_cache_entry
#define bps_tree_key_t struct vy_entry
#define bps_tree_arg_t struct key_def *
#define BPS_TREE_IS_IDENTICAL(a, b) vy_entry_is_equal((a).entry, (b).entry)

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_EXTENT_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t
#undef BPS_TREE_IS_IDENTICAL

/* }}} vy_cache_entry */

/* {{{ vy_cache_drain */

/** Dropped caches, their detached trees pending release. */
struct vy_cache_drain {
	/** The queue of dropped caches, oldest first. */
	struct rlist queue;
};

/* }}} vy_cache_drain */

/* {{{ vy_cache_env */

struct vy_cache_builder;

/**
 * Environment of the cache
 */
struct vy_cache_env {
	/** The circuit of the eviction hand: every live cache. */
	struct rlist gc_list;
	/**
	 * The cache the eviction hand currently sweeps, or NULL if
	 * there are no caches.
	 */
	struct vy_cache *gc_cache;
	/**
	 * The draining of dropped caches. Draining is a process
	 * of its own, separate from the CLOCK eviction; the two
	 * are joined in the eviction walk.
	 */
	struct vy_cache_drain drain;
	/** The scan id counter, see vy_cache_entry::scan_id. */
	uint64_t scan_id;
	/**
	 * The transaction manager, for the read view horizon at
	 * link formation. NULL in unit tests: the clamp is an
	 * optimization, not a correctness requirement.
	 */
	struct vy_tx_manager *xm;
	/**
	 * The empty key: the dup source for the key space end
	 * bounds ([]- and []+). A range reader that runs out of
	 * tuples ends its chain at a copy of it. Owned by the
	 * environment.
	 */
	struct vy_entry empty_key;
	/** Common matras extent allocator. */
	struct matras_allocator allocator;
	/** Size of memory occupied by cached tuples */
	size_t mem_used;
	/**
	 * The resident primary tuples: their mean size is the
	 * eviction hand's weighting unit, see vy_cache_env_gc().
	 */
	struct vy_stmt_counter tuple;
	/** Max memory size that can be used for cache */
	size_t mem_quota;
	/**
	 * The shadow map: the protected epoch pair and the
	 * doorkeeper below, three bloom filters over the keys
	 * of recorded reads, see the vy_cache.c section. NULL,
	 * with a zero mask, while the cache is disabled.
	 */
	uint64_t *shadow[2];
	/** Bits per map array minus one: the index mask. */
	uint64_t shadow_mask;
	/** The index of the young epoch array in shadow[]. */
	unsigned shadow_epoch;
	/** Set bits in the young array; drives the epoch flip. */
	uint64_t shadow_set_bits;
	/** The doorkeeper: first-touch keys, wiped per window. */
	uint64_t *door;
	/** Keys stamped in the doorkeeper; drives its wipe. */
	uint64_t door_keys;
	/** Set bits in the doorkeeper; the wipe's FP backstop. */
	uint64_t door_set_bits;
	/** Bytes walked armed since a flip; drives the flip too. */
	size_t armed_walked;
};

/**
 * Initialize common cache environment.
 * @param e - the environment.
 * @param key_format - the format for the environment's empty key.
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
NODISCARD int
vy_cache_env_create(struct vy_cache_env *env,
		    struct tuple_format *key_format);

/**
 * Destroy and free resources of cache environment.
 * @param e - the environment.
 */
void
vy_cache_env_destroy(struct vy_cache_env *e);

/**
 * Set memory limit for the cache.
 * @param e - the environment.
 * @param quota - memory limit for the cache.
 *
 * The new limit takes effect immediately: admissions gate on it
 * from this point on. The standing overage is then reclaimed in
 * bounded eviction steps, yielding between them so other fibers
 * keep running; the function returns once the cache fits the
 * new limit.
 */
void
vy_cache_env_set_quota(struct vy_cache_env *e, size_t quota);

/* }}} vy_cache_env */

/* {{{ vy_cache */

/**
 * Tuple cache (of one particular LSM tree)
 */
struct vy_cache {
	/**
	 * Key definition for tuple comparison, includes primary
	 * key parts
	 */
	struct key_def *cmp_def;
	/** Set if this cache is for a primary index. */
	bool is_primary;
	/* Tree of cache entries */
	struct vy_cache_tree *tree;
	/* The version of state of the tree. Increments on every change */
	uint32_t version;
	/* Quota, read view horizon, scan id counter, empty key. */
	struct vy_cache_env *env;
	/* Link in vy_cache_env->gc_list. */
	struct rlist in_gc_list;
	/*
	 * The eviction hand's position in this cache: the key to
	 * resume the CLOCK sweep from, none to start from the first
	 * element. The statement is referenced and may outlive its
	 * element.
	 */
	struct vy_entry gc_pos;
	/* Cache statistics. */
	struct vy_cache_stat stat;
};

/**
 * Allocate and initialize tuple cache.
 * @param env - pointer to common cache environment.
 * @param cmp_def - key definition for tuple comparison.
 * @param is_primary - set if cache is for primary index
 */
void
vy_cache_create(struct vy_cache *cache, struct vy_cache_env *env,
		struct key_def *cmp_def, bool is_primary);

/**
 * Destroy and deallocate tuple cache. A non-empty cache is
 * drained asynchronously: its tree is detached onto the
 * environment's drain queue. The eviction walk releases the
 * entries in bounded batches ahead of sweeping the live caches,
 * so dropping a large cache does not stall the caller.
 * @param cache - pointer to tuple cache to destroy.
 */
void
vy_cache_destroy(struct vy_cache *cache);

/**
 * The statement may not be its row's latest committed version:
 * a secondary cache tuple whose row is no longer cached in the
 * primary, where a write would find -- and retire -- it. Never
 * served; its entry is collected on encounter. A primary entry
 * is current as long as it is cached, and the cache's own
 * metadata -- bounds and DELETE records -- has no row to go
 * stale: neither is ever maybe stale. Testing VY_STMT_STALE is
 * not needed either: a stale-marked statement is refused
 * admission and never becomes resident, and the prepare of a
 * newer version retires the old statement before any reader
 * could mark it.
 */
bool
vy_stmt_maybe_stale(struct tuple *stmt, bool is_primary);

/**
 * Cache a point lookup result as a standalone cache entry. No link
 * forms and no builder is needed: a full-key EQ hit is the whole
 * answer structurally, valid for any reader that sees the result's
 * LSN.
 *
 * The admission invariant is checked here: the result must be the
 * newest committed version of its key. The proof is the absence
 * of the staleness witness VY_STMT_STALE, which marks a result a
 * newer version was skipped over -- one invisible in the reader's
 * view, or a prepared one at any view. Prepared but not committed
 * data is refused. A NULL result carries no statement to hold and
 * is not recorded in any form.
 *
 * @param cache the cache to populate.
 * @param entry the point lookup result, or none.
 */
void
vy_cache_add(struct vy_cache *cache, struct vy_entry entry);

/**
 * Find a statement in the cache.
 * @param cache the cache to look up.
 * @param key the key to look for.
 * @return the cached entry of @a key, or none if not cached.
 */
struct vy_entry
vy_cache_get(struct vy_cache *cache, struct vy_entry key);

/**
 * Invalidate possibly cached value due to its overwriting
 * @param cache - pointer to tuple cache.
 * @param entry - overwritten statement.
 * @param[out] found - if not NULL, set to the invalidated
 *             cached statement, referenced.
 */
void
vy_cache_on_write(struct vy_cache *cache, struct vy_entry entry,
		  struct vy_entry *found);

/**
 * Invalidate cache on statement rollback.
 * @param cache - pointer to tuple cache.
 * @param entry - rolled back statement.
 */
void
vy_cache_on_rollback(struct vy_cache *cache, struct vy_entry entry);

/**
 * Remove a statement the reader has proven dead from the cache: a
 * secondary index entry whose primary row was deleted, with the
 * secondary DELETE deferred. Unlike ordinary eviction, the removal
 * keeps the neighbors' pending links: the dead entry stays in the
 * secondary LSM tree until compaction, so a reader whose pending
 * link spans it receives the key from the deeper sources and
 * proves it dead through the primary index.
 *
 * For a point lookup this removal is the whole reaction to a
 * proven-dead key: no DELETE is recorded in place of the removed
 * entry, since that would only speed up repeated point reads of
 * a dead key, which are rare. A range scan does record the
 * DELETE, see vy_cache_builder_add_delete(), so its chain skips
 * the dead key on repeat scans.
 */
void
vy_cache_evict(struct vy_cache *cache, struct vy_entry entry);

/* }}} vy_cache */

/* {{{ vy_cache_builder */

/**
 * Populates the cache with one reader's results: adds every point
 * in key order and links adjacent points into a chain. Holds the
 * last added point and the scan id that stamps its pending
 * links. Owned by the cache API: callers pass it
 * through the vy_cache_builder_* calls and never look inside.
 */
struct vy_cache_builder {
	/** The cache being populated. */
	struct vy_cache *cache;
	/** The key order the points arrive in. */
	enum iterator_type order;
	/** The read's search key, referenced. */
	struct vy_entry key;
	/**
	 * The scan's start bound: the search key -- the resume
	 * key for a resumed scan -- marked with the side the scan
	 * starts from, which a shared, unmarked key cannot
	 * express. It is the entry the chain grows from,
	 * and it positions a from-the-start scan's first
	 * seek. The bound is
	 * scan-scoped state: it outlives the cache iterator,
	 * which is closed and reopened on every source restart.
	 * Owns a reference; none only on creation failure, when
	 * the cache serves nothing and the chain has no start
	 * bound.
	 */
	struct vy_entry start_bound;
	/**
	 * The scan's end bound: the far side of the search key's
	 * match range for an EQ reader, the far end of the key
	 * space for a range reader. Made at creation, inserted at
	 * close, when a completed walk links its last result to
	 * it (see vy_cache_builder_close()). Owns a reference.
	 * None in three cases: the cache is disabled and no
	 * chain is built; a full-key EQ, which inserts no bounds;
	 * creation failure, when the chain simply ends unlinked.
	 */
	struct vy_entry end_bound;
	/** The chain's frontier: the last added point, referenced. */
	struct vy_entry last;
	/**
	 * The frontier's tree position, refreshed on every
	 * frontier advance. The tree reshapes under concurrent
	 * admissions and evictions, so the position is trusted
	 * only after revalidation: the element it addresses must
	 * still hold vy_cache_builder::last's statement (see
	 * vy_cache_iterator_restore()).
	 */
	struct vy_cache_tree_iterator last_pos;
	/**
	 * The number of results the reader produced onto the
	 * chain -- statements it recorded plus entries served to
	 * it over existing links (see vy_cache_builder_on_read()).
	 * vy_cache_builder::last alone cannot tell whether there
	 * were any: a chain that has not moved still points at
	 * its start bound -- or at the neighboring tuple the
	 * start bound fused into (see the fusing in
	 * vy_cache_link()).
	 */
	uint32_t chain_length;
	/** The scan's id, stamped on the entries the chain touches. */
	uint64_t scan_id;
#ifndef NDEBUG
	/** LINKLOG: the scan's read view, for its trace. */
	const struct vy_read_view **rv;
#endif
};

/**
 * Initialize a builder: issue its scan id and open the chain's
 * leading pending link. A read from the start inserts the
 * bound key of its search key and opens the pending link on it
 * (a full-key EQ does not insert it, the bound key only
 * positions the cache iterator). A read resumed after @a last
 * inserts the bound key of the resume position instead: the
 * range before it was not observed, and nothing is recorded
 * about it. The
 * builder is created before the reader's first advance, and so
 * before its first yield: the first added point links back
 * unless a write in between destroys the pending link.
 */
void
vy_cache_builder_create(struct vy_cache_builder *builder,
			struct vy_cache *cache, enum iterator_type order,
			struct vy_entry key, struct vy_entry last);

/** Release the builder. */
void
vy_cache_builder_destroy(struct vy_cache_builder *builder);

/**
 * Add the reader's next result -- a live tuple -- to the chain:
 * the point enters the cache and, if the pending link opened at
 * the previous point survived, the link between the two is
 * completed. A none result is the end of matches: the chain is
 * completed with the bound key it ends at -- the search key for
 * an EQ reader, the empty key for a range reader that ran out of
 * tuples. A stale result -- one shadowed by a version above the
 * reader's view, per @a is_stale or the VY_STMT_STALE flag --
 * is not the latest: it is not added, and the chain breaks
 * instead. Consumed DELETEs take their own path,
 * vy_cache_builder_add_delete().
 * @param builder the reader's chain.
 * @param curr the result, none at the end of matches.
 * @param is_stale the advance that produced the result crossed
 *        a statement above the reader's view, see
 *        vy_read_iterator::is_stale.
 */
void
vy_cache_builder_add(struct vy_cache_builder *builder, struct vy_entry curr,
		     bool is_stale);

/**
 * Record a consumed DELETE in the chain. A DELETE at or below
 * the read view horizon serves no reader that can ever exist:
 * it is not recorded, and the chain is not broken either -- the
 * neighbors link up across the dead key. A DELETE fuses into
 * the previous chain entry when that entry is a DELETE or a
 * bound key: its LSN rises instead of a second entry entering
 * the cache, so consecutive dead keys cost one entry however
 * many there are -- or none, when a scan's own start bound
 * absorbs them.
 *
 * A below-horizon refusal also drops the stale cached row, if
 * any, because nothing else ever will. E.g. a secondary index
 * caches tuple {1, 10}; the primary row is then deleted with
 * the secondary DELETE deferred, so this cache is not
 * invalidated and still holds {1, 10}; a later scan resolves
 * the key through the primary index and finds it dead. When no
 * reader is old enough to see the row, the DELETE itself is
 * not worth recording, but the stale {1, 10} must be dropped
 * right here: no write ever lands on this key in this index
 * again.
 *
 * The key may also be proven dead by a prepared but not
 * committed transaction: a reader that sees prepared data can
 * resolve a secondary tuple through the primary index and find
 * the row overwritten by a transaction that has not committed
 * yet. Such a death can still be undone: if that transaction
 * rolls back, the key is alive again, and the rollback does not
 * invalidate this cache -- the secondary DELETE is deferred and
 * was never written anywhere. So the DELETE is not recorded,
 * and the chain breaks: a link over the key would claim it
 * absent while it may come back.
 *
 * @param builder the reader's chain.
 * @param delete_key the dead key statement.
 * @param delete_lsn the LSN of the statement that deleted the
 * key, 0 if unknown.
 * @param is_stale the advance that produced the verdict crossed
 * a statement above the reader's view: the chain breaks, see
 * vy_cache_builder_add().
 */
void
vy_cache_builder_add_delete(struct vy_cache_builder *builder,
			    struct vy_entry delete_key,
			    int64_t delete_lsn, bool is_stale);

/**
 * Register a cache entry the reader produced: the entry becomes
 * the chain's frontier and carries the chain's pending link, so
 * the next entry the reader produces links back to it. The
 * cache iterator calls it for an entry served over observed
 * links: such an entry is already cached and linked, so the
 * chain has nothing else to do (see vy_cache_iterator_open()).
 * vy_cache_insert() calls it for an entry the reader adds.
 * @param builder the reader's chain.
 * @param node the entry.
 * @param pos the entry's tree position, remembered as the
 *        frontier's home (see vy_cache_builder::last_pos).
 */
void
vy_cache_builder_on_read(struct vy_cache_builder *builder,
			 struct vy_cache_entry *node,
			 struct vy_cache_tree_iterator pos);

/**
 * No link may span this point of the chain: forget the last
 * added point, so the next added point links to nothing.
 */
void
vy_cache_builder_break_link(struct vy_cache_builder *builder);

/* }}} vy_cache_builder */

/* {{{ vy_cache_iterator */


/**
 * Cache iterator
 */
struct vy_cache_iterator {
	/* The cache */
	struct vy_cache *cache;

	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if the key is not specified, GT and EQ are changed to
	 * GE, LT to LE for beauty.
	 */
	enum iterator_type iterator_type;
	/* Search key data in terms of vinyl, vy_entry_compare argument */
	struct vy_entry key;
	/* LSN visibility, iterator shows values with lsn <= vlsn */
	const struct vy_read_view **read_view;
	/*
	 * The reader's chain, borrowed from the read iterator
	 * that owns both. The first seek takes the scan's start
	 * bound from it, and an entry served over observed links
	 * is reported to it (see vy_cache_builder_on_read()), so
	 * the chain rides the existing links instead of
	 * re-establishing them.
	 */
	struct vy_cache_builder *builder;

	/* State of iterator */
	/* Current position in tree */
	struct vy_cache_tree_iterator curr_pos;
	/* stmt in current position in tree */
	struct vy_entry curr;

	/*
	 * The cache version at the last search, zero until the
	 * first one: version zero predates every cache.
	 */
	uint32_t version;
};

/**
 * Open an iterator over cache.
 * @param itr - iterator to open.
 * @param cache - the cache.
 * @param iterator_type - iterator type (EQ, GT, GE, LT, LE or ALL)
 * @param key - search key data in terms of vinyl, vy_entry_compare argument
 * @param rv - read view.
 * @param builder - the reader's chain to report served entries
 * to, NULL when the reader builds none.
 */
void
vy_cache_iterator_open(struct vy_cache_iterator *itr, struct vy_cache *cache,
		       enum iterator_type iterator_type, struct vy_entry key,
		       const struct vy_read_view **rv,
		       struct vy_cache_builder *builder);

/**
 * Advance a cache iterator to the next key.
 * @param itr the iterator.
 * @param[out] history the key history, empty at the end of
 * matches.
 * @param[out] stop set if the iterator observed an unbroken
 * sequence of links up to the returned key, so no statement can
 * precede it in deeper sources.
 * @retval 0 success.
 * @retval -1 memory allocation error.
 */
NODISCARD int
vy_cache_iterator_next(struct vy_cache_iterator *itr,
		       struct vy_history *history, bool *stop);

/**
 * Advance a cache iterator to the first key following @a last.
 * @param itr the iterator.
 * @param last the key to advance past, or none to start from the
 * search key.
 * @param[out] history the key history, empty at the end of
 * matches.
 * @param[out] stop see vy_cache_iterator_next().
 * @retval 0 success.
 * @retval -1 memory allocation error.
 */
NODISCARD int
vy_cache_iterator_skip(struct vy_cache_iterator *itr, struct vy_entry last,
		       struct vy_history *history, bool *stop);

/**
 * Check if a cache iterator was invalidated and needs to be
 * restored.
 * @param itr the iterator.
 * @param last the key to restore after.
 * @param[out] history the key history at the restored position.
 * @param[out] stop see vy_cache_iterator_next().
 * @retval 1 the iterator was restored to the first key following
 * @a last.
 * @retval 0 the position is still valid.
 * @retval -1 memory allocation error.
 */
NODISCARD int
vy_cache_iterator_restore(struct vy_cache_iterator *itr, struct vy_entry last,
			  struct vy_history *history, bool *stop);

/**
 * Close a cache iterator.
 * @param itr the iterator to close.
 */
void
vy_cache_iterator_close(struct vy_cache_iterator *itr);

/* }}} vy_cache_iterator */

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_CACHE_H */
