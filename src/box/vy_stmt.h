#ifndef INCLUDES_TARANTOOL_BOX_VY_STMT_H
#define INCLUDES_TARANTOOL_BOX_VY_STMT_H
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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

#include <trivia/util.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <msgpuck.h>
#include <bit/bit.h>
#include <small/small.h>

#include "tuple.h"
#include "iproto_constants.h"
#include "iterator_type.h"
#include "vy_entry.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct xrow_header;
struct region;
struct tuple_format;
struct tuple_dictionary;
struct tuple_bloom;
struct tuple_bloom_builder;
struct iovec;

#define MAX_LSN (INT64_MAX / 2)

enum {
	VY_UPSERT_THRESHOLD = 128,
	VY_UPSERT_INF,
};
static_assert(VY_UPSERT_THRESHOLD <= UINT8_MAX, "n_upserts max value");
static_assert(VY_UPSERT_INF == VY_UPSERT_THRESHOLD + 1,
	      "inf must be threshold + 1");

struct vy_mem_env;

/** Vinyl statement environment. */
struct vy_stmt_env {
	/** Vinyl statement vtable. */
	struct tuple_format_vtab tuple_format_vtab;
	/**
	 * Mem env whose drain queue is poked on every TX-thread tuple
	 * allocation. vy_stmt_env_create leaves it NULL; the engine
	 * (and the unit-test helper) wire it before allocating any
	 * tuple, so a main-thread allocation always finds it set.
	 */
	struct vy_mem_env *mem_env;
	/**
	 * Max tuple size
	 * @see box.cfg.vinyl_max_tuple_size
	 */
	size_t max_tuple_size;
	/**
	 * Slab allocator for vinyl tuples in the TX thread. Backed
	 * by the mem env's slab cache -- the same dedicated vinyl
	 * arena that holds the in-memory tree extents -- so vinyl
	 * tuple memory is isolated from the runtime allocator.
	 */
	struct small_alloc tx_alloc;
	/**
	 * Tuple format used for creating key statements (e.g.
	 * statements read from secondary index runs). It doesn't
	 * impose any restrictions on tuple fields, neither does
	 * it setup offset map.
	 *
	 * Note, all key statements must use this format, because
	 * vy_stmt_is_key() is built upon that assumption.
	 */
	struct tuple_format *key_format;
};

/** Initialize a vinyl statement environment. */
void
vy_stmt_env_create(struct vy_stmt_env *env, struct vy_mem_env *mem_env);

/** Destroy a vinyl statement environment. */
void
vy_stmt_env_destroy(struct vy_stmt_env *env);

/** Return memory used by vinyl tuples in the TX thread. */
size_t
vy_stmt_tuple_memory_used(struct vy_stmt_env *env);

/**
 * Constant-time check whether a statement of the given format holding
 * tuple_len bytes of MessagePack data would fit within the environment's
 * max_tuple_size. Uses the format's field_map_size_max instead of walking
 * the fields; the bound is exact unless the format has a multikey index.
 * Sets the diag on overflow.
 *
 * @retval 0  the statement fits
 * @retval -1 the statement is too large (diag is set)
 */
int
vy_stmt_check_size(const struct vy_stmt_env *env,
		   const struct tuple_format *format,
		   size_t tuple_len);

/**
 * Initialize the per-thread slab allocator for vinyl tuples.
 * Must be called in each background cord before any tuple allocations.
 */
void
vy_stmt_init_thread_alloc(struct small_alloc *alloc);

/**
 * Destroy the per-thread slab allocator.
 * Called when a background cord shuts down.
 */
void
vy_stmt_destroy_thread_alloc(struct small_alloc *alloc);

/** Create a simple vinyl statement format. */
struct tuple_format *
vy_simple_stmt_format_new(struct vy_stmt_env *env,
			  struct key_def *const *keys, uint16_t key_count);

/** Statement flags. */
enum {
	/**
	 * A REPLACE/DELETE request is supposed to delete the old
	 * tuple from all indexes. In order to generate a DELETE
	 * statement for a secondary index, we need to look up the
	 * old tuple in the primary index, which is expensive as
	 * it implies a random disk access. We can optimize out the
	 * lookup by deferring generation of the DELETE statement
	 * until primary index compaction.
	 *
	 * The following flag is set for those REPLACE and DELETE
	 * statements that skipped deletion of the old tuple from
	 * secondary indexes. It makes the write iterator generate
	 * DELETE statements for them during compaction.
	 */
	VY_STMT_DEFERRED_DELETE		= 1 << 0,
	/**
	 * Statements that have this flag set are ignored by the
	 * read iterator.
	 *
	 * We set this flag for deferred DELETE statements, because
	 * they may violate the invariant which the read relies upon:
	 * the older a source, the older statements it stores for a
	 * particular key.
	 */
	VY_STMT_SKIP_READ		= 1 << 1,
	/**
	 * This flag is set for those REPLACE statements that were
	 * generated by UPDATE operations. It is used by the write
	 * iterator to turn such REPLACEs into INSERTs in secondary
	 * indexes so that they can get annihilated with DELETEs on
	 * compaction. It is never written to disk.
	 */
	VY_STMT_UPDATE			= 1 << 2,
	/**
	 * Bit mask of all statement flags.
	 */
	VY_STMT_FLAGS_ALL = (VY_STMT_DEFERRED_DELETE | VY_STMT_SKIP_READ |
			     VY_STMT_UPDATE),
	/**
	 * The key statement is the infimum of its key: it sorts
	 * before any equal key. Marks a slice's exclusive upper
	 * bound (data_end clipped by a range boundary) and the
	 * cache's key entries. Transient: never persisted, not
	 * included in VY_STMT_FLAGS_ALL.
	 */
	VY_STMT_INFIMUM			= 1 << 3,
	/**
	 * The statement contributes to the primary index's per-type
	 * counters used by space:len(). Decided at prepare and cached
	 * so commit avoids a tree lookup. Transient: never persisted,
	 * not included in VY_STMT_FLAGS_ALL.
	 */
	VY_STMT_COUNTED			= 1 << 4,
	/**
	 * Set on a statement when a point lookup finds a newer
	 * version that is invisible in the current read view.
	 * Tells the caller that this is not the latest version
	 * and must not be cached.
	 * Transient: never persisted, not included in
	 * VY_STMT_FLAGS_ALL.
	 */
	VY_STMT_STALE			= 1 << 5,
	/**
	 * The statement is resident in the in-memory level: its bytes
	 * have been charged to the memory quota. A committed statement
	 * is shared across the primary mem and every secondary mem of
	 * its space (one slab tuple, many tree refs), so this marks the
	 * first mem it enters and the charge is made once -- later
	 * inserts of the same shared statement skip it. A sole-owned
	 * secondary statement (a deferred-delete tombstone, a build key)
	 * is a distinct object with the flag unset, so it is charged in
	 * its own right, keeping it visible to the scheduler.
	 *
	 * Never cleared: vinyl always allocates a fresh statement for a
	 * write (vy_tx_set and the surrogate/build/dup paths), so a
	 * statement is never reused for a later write -- the flag dies
	 * with the statement at refcount 0 and cannot go stale.
	 * Transient: never persisted, not included in VY_STMT_FLAGS_ALL.
	 */
	VY_STMT_MEM			= 1 << 6,
	/**
	 * The key statement is the supremum of its key: it sorts
	 * after any equal key. The counterpart of VY_STMT_INFIMUM,
	 * used by the cache's key entries. Transient: never
	 * persisted, not included in VY_STMT_FLAGS_ALL.
	 */
	VY_STMT_SUPREMUM		= 1 << 7,
};

/**
 * A vinyl statement can have either key or tuple format.
 *
 * Tuple statement structure:
 *                               data_offset
 *                                    ^
 * +----------------------------------+
 * |               4 bytes      4 bytes     MessagePack data.
 * |               +------+----+------+---------------------------+- - - - - - .
 *tuple, ..., raw: | offN | .. | off1 | header ..|key1|..|keyN|.. | operations |
 *                 +--+---+----+--+---+---------------------------+- - - - - - .
 *                 |     ...    |                 ^       ^
 *                 |            +-----------------+       |
 *                 +--------------------------------------+
 * Offsets are stored only for indexed fields, though MessagePack'ed tuple data
 * can contain also not indexed fields. For example, if fields 3 and 5 are
 * indexed then before MessagePack data are stored offsets only for field 3 and
 * field 5.
 *
 * Key statement structure:
 * +--------------+-----------------+
 * | array header | part1 ... partN |  -  MessagePack data
 * +--------------+-----------------+
 *
 * Field 'operations' is used for storing operations of UPSERT statement.
 */
struct vy_stmt {
	struct tuple base;
	uint8_t type; /* IPROTO_INSERT/REPLACE/UPSERT/DELETE */
	/**
	 * Upserts count of the vinyl statement.
	 * Only used for UPSERT statements allocated on lsregion, but always in
	 * present in structure since it costs nothing.
	 */
	uint8_t n_upserts;
	/*
	 * The flag word. Only the bits of VY_STMT_FLAGS_ALL are
	 * ever persisted; the rest are runtime state and fit
	 * above the persisted range.
	 */
	uint16_t flags;
	int64_t lsn;
	/**
	 * Offsets array concatenated with MessagePack fields
	 * array.
	 * char raw[0];
	 */
};

static_assert(sizeof(struct vy_stmt) == 24, "Just to be sure");

/** Get LSN of the vinyl statement. */
static inline int64_t
vy_stmt_lsn(struct tuple *stmt)
{
	return ((struct vy_stmt *) stmt)->lsn;
}

/** Set LSN of the vinyl statement. */
static inline void
vy_stmt_set_lsn(struct tuple *stmt, int64_t lsn)
{
	((struct vy_stmt *) stmt)->lsn = lsn;
}

/**
 * Return true if the given LSN is a PLSN, i.e. an LSN used for a prepared
 * (not yet written to WAL) statement.
 */
static inline bool
vy_lsn_is_prepared(int64_t lsn)
{
	return lsn >= MAX_LSN;
}

/** Return true if the statement was prepared, but not yet written to WAL. */
static inline bool
vy_stmt_is_prepared(struct tuple *stmt)
{
	return vy_lsn_is_prepared(vy_stmt_lsn(stmt));
}

/** Get type of the vinyl statement. */
static inline enum iproto_type
vy_stmt_type(struct tuple *stmt)
{
	return (enum iproto_type)((struct vy_stmt *) stmt)->type;
}

/** Set type of the vinyl statement. */
static inline void
vy_stmt_set_type(struct tuple *stmt, enum iproto_type type)
{
	((struct vy_stmt *) stmt)->type = type;
}

/** Get flags of the vinyl statement. */
static inline uint16_t
vy_stmt_flags(struct tuple *stmt)
{
	return ((struct vy_stmt *)stmt)->flags;
}

/** Set flags of the vinyl statement. */
static inline void
vy_stmt_set_flags(struct tuple *stmt, uint16_t flags)
{
	assert(flags == 0 || ((struct vy_stmt *)stmt)->flags == 0);
	((struct vy_stmt *)stmt)->flags = flags;
}

/** OR one or more flags into the vinyl statement's flag word. */
static inline void
vy_stmt_add_flag(struct tuple *stmt, uint16_t flag)
{
	((struct vy_stmt *)stmt)->flags |= flag;
}

/** Clear one or more flags in the vinyl statement's flag word. */
static inline void
vy_stmt_del_flag(struct tuple *stmt, uint16_t flag)
{
	((struct vy_stmt *)stmt)->flags &= ~flag;
}

/** Check if the vinyl statement has a given flag set. */
static inline bool
vy_stmt_has_flag(struct tuple *stmt, uint16_t flag)
{
	return vy_stmt_flags(stmt) & flag;
}

/** Check if the entry is the infimum of its key. */
static inline bool
vy_entry_is_infimum(struct vy_entry entry)
{
	return vy_stmt_flags(entry.stmt) & VY_STMT_INFIMUM;
}

/**
 * Get upserts count of the vinyl statement.
 * Only meaningful for UPSERT statements sitting in a mem tree.
 */
static inline uint8_t
vy_stmt_n_upserts(struct tuple *stmt)
{
	assert(vy_stmt_type(stmt) == IPROTO_UPSERT);
	return ((struct vy_stmt *)stmt)->n_upserts;
}

/**
 * Set upserts count of the vinyl statement.
 * Only for UPSERT statements allocated on lsregion or for raw initialization,
 * when all fields one by one are set to zeros.
 */
static inline void
vy_stmt_set_n_upserts(struct tuple *stmt, uint8_t n)
{
	assert(vy_stmt_type(stmt) == IPROTO_UPSERT || n == 0);
	((struct vy_stmt *)stmt)->n_upserts = n;
}

/** Return true if the given format is a key format. */
static inline bool
vy_stmt_is_key_format(const struct tuple_format *format)
{
	struct vy_stmt_env *env = format->engine;
	return env->key_format == format;
}

/** Return true if the vinyl statement has key format. */
static inline bool
vy_stmt_is_key(struct tuple *stmt)
{
	return vy_stmt_is_key_format(tuple_format(stmt));
}

/**
 * Return the number of key parts defined in the given vinyl
 * statement.
 *
 * If the statement represents a tuple, we assume that it has
 * all key parts defined.
 */
static inline uint32_t
vy_stmt_key_part_count(struct tuple *stmt, struct key_def *key_def)
{
	if (vy_stmt_is_key(stmt)) {
		uint32_t part_count = tuple_field_count(stmt);
		assert(part_count <= key_def->part_count);
		return part_count;
	}
	return key_def->part_count;
}

/**
 * Return true if the given vinyl statement contains all
 * key parts, i.e. can be used for an exact match lookup.
 */
static inline bool
vy_stmt_is_full_key(struct tuple *stmt, struct key_def *key_def)
{
	return vy_stmt_key_part_count(stmt, key_def) == key_def->part_count;
}

/**
 * Return true if the given vinyl statement stores an empty
 * (match all) key.
 */
static inline bool
vy_stmt_is_empty_key(struct tuple *stmt)
{
	return tuple_field_count(stmt) == 0;
}

/**
 * Return true if there cannot be more than one tuple equal to
 * the given vinyl statement in an index.
 */
bool
vy_stmt_is_exact_key(struct tuple *stmt, struct key_def *cmp_def,
		     struct key_def *key_def, bool is_unique);

/**
 * Duplicate the statement, flags included: a copy of a bound
 * key is the same bound.
 *
 * @param stmt statement
 * @return new statement of the same type with the same data.
 */
struct tuple *
vy_stmt_dup(struct tuple *stmt);

/**
 * Return true if @a stmt can be referenced on the current cord.
 * Tx-local tuples (TUPLE_TX_LOCAL) carry a non-atomic refcount
 * that may only be touched on the tx (main) cord; worker cords
 * (dump / compaction / reader) hold them alive via their sealed
 * mem's +1 instead. Tuples allocated by a worker cord stay
 * private to the allocating cord and are always refable there.
 * @param stmt a statement
 * @retval true if @a stmt's refcount may be mutated here
 * @retval false otherwise
 */
static inline bool
vy_stmt_is_refable(struct tuple *stmt)
{
	return !tuple_has_flag(stmt, TUPLE_TX_LOCAL) ||
	       cord_is_main_dont_create();
}

/**
 * Bump @a stmt's refcount, asserting the current cord is allowed
 * to touch it. Use this in place of plain tuple_ref() on any
 * vinyl statement: the assert documents the cord contract and
 * fires loudly the day a caller is wired up from a worker cord
 * holding a TUPLE_TX_LOCAL stmt.
 */
static inline void
vy_stmt_ref(struct tuple *stmt)
{
	assert(vy_stmt_is_refable(stmt));
	tuple_ref(stmt);
}

/** Mirror of vy_stmt_ref. */
static inline void
vy_stmt_unref(struct tuple *stmt)
{
	assert(vy_stmt_is_refable(stmt));
	tuple_unref(stmt);
}

/**
 * Ref tuple, if it exists (!= NULL) and can be referenced.
 * @sa vy_stmt_is_refable.
 *
 * @param tuple Tuple to ref or NULL.
 */
static inline void
vy_stmt_ref_if_possible(struct tuple *stmt)
{
	if (vy_stmt_is_refable(stmt))
		tuple_ref(stmt);
}

/**
 * Unref tuple, if it exists (!= NULL) and can be unreferenced.
 * @sa vy_stmt_is_refable.
 *
 * @param tuple Tuple to unref or NULL.
 */
static inline void
vy_stmt_unref_if_possible(struct tuple *stmt)
{
	if (vy_stmt_is_refable(stmt))
		tuple_unref(stmt);
}

/**
 * Return a comparison hint of a vinyl statement.
 */
static inline hint_t
vy_stmt_hint(struct tuple *stmt, struct key_def *key_def)
{
	if (vy_stmt_is_key(stmt)) {
		const char *key = tuple_data(stmt);
		uint32_t part_count = mp_decode_array(&key);
		return key_hint(key, part_count, key_def);
	} else {
		return tuple_hint(stmt, key_def);
	}
}

/**
 * Compare two keys with MessagePack array header.
 */
static inline int
vy_key_compare(const char *key_a, hint_t key_a_hint,
	       const char *key_b, hint_t key_b_hint,
	       struct key_def *key_def)
{
	uint32_t part_count_a = mp_decode_array(&key_a);
	uint32_t part_count_b = mp_decode_array(&key_b);
	return key_compare(key_a, part_count_a, key_a_hint,
			   key_b, part_count_b, key_b_hint, key_def);
}

/**
 * Compare two vinyl statements taking into account their
 * formats (key or tuple) and using comparison hints.
 */
static inline int
vy_stmt_compare(struct tuple *a, hint_t a_hint,
		struct tuple *b, hint_t b_hint,
		struct key_def *key_def)
{
	bool a_is_tuple = !vy_stmt_is_key(a);
	bool b_is_tuple = !vy_stmt_is_key(b);
	if (a_is_tuple && b_is_tuple) {
		return tuple_compare(a, a_hint, b, b_hint, key_def);
	} else if (a_is_tuple && !b_is_tuple) {
		const char *key = tuple_data(b);
		uint32_t part_count = mp_decode_array(&key);
		return tuple_compare_with_key(a, a_hint, key, part_count,
					      b_hint, key_def);
	} else if (!a_is_tuple && b_is_tuple) {
		const char *key = tuple_data(a);
		uint32_t part_count = mp_decode_array(&key);
		return -tuple_compare_with_key(b, b_hint, key, part_count,
					       a_hint, key_def);
	} else {
		assert(!a_is_tuple && !b_is_tuple);
		return vy_key_compare(tuple_data(a), a_hint,
				      tuple_data(b), b_hint, key_def);
	}
}

/**
 * Compare a vinyl statement (key or tuple) with a raw key
 * (msgpack array) using comparison hints.
 */
static inline int
vy_stmt_compare_with_raw_key(struct tuple *stmt, hint_t stmt_hint,
			     const char *key, hint_t key_hint,
			     struct key_def *key_def)
{
	if (!vy_stmt_is_key(stmt)) {
		uint32_t part_count = mp_decode_array(&key);
		return tuple_compare_with_key(stmt, stmt_hint, key,
					      part_count, key_hint,
					      key_def);
	}
	return vy_key_compare(tuple_data(stmt), stmt_hint,
			      key, key_hint, key_def);
}

/**
 * Create a key statement from raw MessagePack data.
 * @param format     Format of an index.
 * @param key        MessagePack data that contain an array of
 *                   fields WITHOUT the array header.
 * @param part_count Count of the key fields that will be saved as
 *                   result.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
struct tuple *
vy_key_new(struct tuple_format *format, const char *key, uint32_t part_count);

/**
 * Create a new surrogate DELETE from @a tuple using @a format.
 * A surrogate tuple has format->field_count fields from the source
 * with all unindexed fields replaced with MessagePack NIL.
 *
 * Example:
 * original:      {a1, a2, a3, a4, a5}
 * index key_def: {2, 4}
 * result:        {null, a2, null, a4, null}
 *
 * @param format Target tuple format.
 * @param src    Source tuple from the primary index.
 *
 * @retval not NULL Success.
 * @retval     NULL Memory or fields format error.
 */
struct tuple *
vy_stmt_new_surrogate_delete_raw(struct tuple_format *format,
				 const char *data, const char *data_end);

/** @copydoc vy_stmt_new_surrogate_delete_raw. */
static inline struct tuple *
vy_stmt_new_surrogate_delete(struct tuple_format *format, struct tuple *tuple)
{
	uint32_t size;
	const char *data = tuple_data_range(tuple, &size);
	return vy_stmt_new_surrogate_delete_raw(format, data, data + size);
}

/**
 * Create the REPLACE statement from raw MessagePack data.
 * @param format Format of a tuple for offsets generating.
 * @param tuple_begin MessagePack data that contain an array of fields WITH the
 *                    array header.
 * @param tuple_end End of the array that begins from @param tuple_begin.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
struct tuple *
vy_stmt_new_replace(struct tuple_format *format, const char *tuple,
                    const char *tuple_end);

/**
 * Create the INSERT statement from raw MessagePack data.
 * @param format Format of a tuple for offsets generating.
 * @param tuple_begin MessagePack data that contain an array of fields WITH the
 *                    array header.
 * @param tuple_end End of the array that begins from @param tuple_begin.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
struct tuple *
vy_stmt_new_insert(struct tuple_format *format, const char *tuple_begin,
		   const char *tuple_end);

/**
 * Create the DELETE statement from raw MessagePack data.
 * @param format Format of a tuple for offsets generating.
 * @param tuple_begin MessagePack data that contain an array of fields WITH the
 *                    array header.
 * @param tuple_end End of the array that begins from @param tuple_begin.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
struct tuple *
vy_stmt_new_delete(struct tuple_format *format, const char *tuple_begin,
		   const char *tuple_end);

 /**
 * Create the UPSERT statement from raw MessagePack data.
 * @param tuple_begin MessagePack data that contain an array of fields WITH the
 *                    array header.
 * @param tuple_end End of the array that begins from @param tuple_begin.
 * @param format Format of a tuple for offsets generating.
 * @param part_count Part count from key definition.
 * @param operations Vector of update operation pieces. Each iovec here may be
 *     a part of an operation, or a whole operation, or something including
 *     several operations. It is just a list of buffers. Each buffer is not
 *     interpreted as an independent operation.
 * @param ops_cnt Length of the update operations vector.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
struct tuple *
vy_stmt_new_upsert(struct tuple_format *format,
		   const char *tuple_begin, const char *tuple_end,
		   struct iovec *operations, uint32_t ops_cnt);

/**
 * Create REPLACE statement from UPSERT statement.
 *
 * @param upsert         Upsert statement.
 * @retval not NULL Success.
 * @retval     NULL Memory error.
 */
struct tuple *
vy_stmt_replace_from_upsert(struct tuple *upsert);

/**
 * Extract MessagePack data from the REPLACE/UPSERT statement.
 * @param stmt An UPSERT or REPLACE statement.
 * @param[out] p_size Size of the MessagePack array in bytes.
 *
 * @return MessagePack array of tuple fields.
 */
static inline const char *
vy_upsert_data_range(struct tuple *tuple, uint32_t *p_size)
{
	assert(vy_stmt_type(tuple) == IPROTO_UPSERT);
	const char *mp = tuple_data(tuple);
	assert(mp_typeof(*mp) == MP_ARRAY);
	const char *mp_end = mp;
	mp_next(&mp_end);
	assert(mp < mp_end);
	*p_size = mp_end - mp;
	return mp;
}

/**
 * Extract the operations array from the UPSERT statement.
 * @param stmt An UPSERT statement.
 * @param mp_size Out parameter for size of the returned array.
 *
 * @retval Pointer on MessagePack array of update operations.
 */
static inline const char *
vy_stmt_upsert_ops(struct tuple *tuple, uint32_t *mp_size)
{
	assert(vy_stmt_type(tuple) == IPROTO_UPSERT);
	const char *mp = tuple_data(tuple);
	mp_next(&mp);
	*mp_size = tuple_data(tuple) + tuple_bsize(tuple) - mp;
	return mp;
}

/**
 * Create a key statement from MessagePack array.
 * @param format  Format of an index.
 * @param key     MessagePack array of key fields.
 *
 * @retval not NULL Success.
 * @retval     NULL Memory error.
 */
static inline struct tuple *
vy_key_from_msgpack(struct tuple_format *format, const char *key)
{
	uint32_t part_count = mp_decode_array(&key);
	return vy_key_new(format, key, part_count);
}

/**
 * Extract the key from a tuple by the given key definition
 * and store the result in a key statement allocated with
 * malloc().
 */
struct tuple *
vy_stmt_extract_key(struct tuple *stmt, struct key_def *key_def,
		    struct tuple_format *format, int multikey_idx);

/**
 * Extract the key from msgpack by the given key definition
 * and store the result in a key statement allocated with
 * malloc().
 */
struct tuple *
vy_stmt_extract_key_raw(const char *data, const char *data_end,
			struct key_def *key_def, struct tuple_format *format,
			int multikey_idx);

/**
 * Add a statement hash to a bloom filter builder.
 * See tuple_bloom_builder_add() for more details.
 */
int
vy_bloom_builder_add(struct tuple_bloom_builder *builder,
		     struct vy_entry entry, struct key_def *key_def);

/**
 * Check if a statement hash is present in a bloom filter.
 * See tuple_bloom_maybe_has() for more details.
 */
bool
vy_bloom_maybe_has(const struct tuple_bloom *bloom,
		   struct vy_entry entry, struct key_def *key_def);

/**
 * Encode vy_stmt for a primary key as xrow_header
 *
 * @param value statement to encode
 * @param key_def key definition
 * @param space_id is written to the request header unless it is 0.
 * Pass 0 to save some space in xrow.
 * @param xrow[out] xrow to fill
 *
 * @retval 0 if OK
 * @retval -1 if error
 */
int
vy_stmt_encode_primary(struct tuple *value, struct key_def *key_def,
		       uint32_t space_id, struct xrow_header *xrow);

/**
 * Encode vy_stmt for a secondary key as xrow_header
 *
 * @param value statement to encode
 * @param key_def key definition
 * @param multikey_idx multikey index hint
 * @param xrow[out] xrow to fill
 *
 * @retval 0 if OK
 * @retval -1 if error
 */
int
vy_stmt_encode_secondary(struct tuple *value, struct key_def *cmp_def,
			 int multikey_idx, struct xrow_header *xrow);

/**
 * Reconstruct vinyl tuple info and data from xrow
 *
 * @retval stmt on success
 * @retval NULL on error
 */
struct tuple *
vy_stmt_decode(struct xrow_header *xrow, struct tuple_format *format);

/**
 * Format a statement into string.
 * Example: REPLACE([1, 2, "string"], lsn=48)
 */
int
vy_stmt_snprint(char *buf, int size, struct tuple *stmt);

/*
 * Format a statement into string using a static buffer.
 * Useful for gdb and say_debug().
 * \sa vy_stmt_snprint()
 */
const char *
vy_stmt_str(struct tuple *stmt);

/**
 * Extract a multikey index hint from a statement entry.
 * Returns MULTIKEY_NONE if the key definition isn't multikey.
 */
static inline int
vy_entry_multikey_idx(struct vy_entry entry, struct key_def *key_def)
{
	if (!key_def->is_multikey || vy_stmt_is_key(entry.stmt))
		return MULTIKEY_NONE;
	assert(entry.hint != HINT_NONE);
	return (int)entry.hint;
}

/**
 * Create a key entry from a MessagePack array without a header.
 */
static inline struct vy_entry
vy_entry_key_new(struct tuple_format *format, struct key_def *key_def,
		 const char *key, uint32_t part_count)
{
	struct vy_entry entry;
	entry.stmt = vy_key_new(format, key, part_count);
	if (entry.stmt == NULL)
		return vy_entry_none();
	entry.hint = key_hint(key, part_count, key_def);
	return entry;
}

/**
 * Create a key entry from a MessagePack array.
 */
static inline struct vy_entry
vy_entry_key_from_msgpack(struct tuple_format *format, struct key_def *key_def,
			  const char *key)
{
	uint32_t part_count = mp_decode_array(&key);
	return vy_entry_key_new(format, key_def, key, part_count);
}

/**
 * Compare the statements stored in the given entries.
 */
static inline int
vy_entry_compare(struct vy_entry a, struct vy_entry b, struct key_def *key_def)
{
	return vy_stmt_compare(a.stmt, a.hint, b.stmt, b.hint, key_def);
}

/**
 * Compare a statement stored in the given entry with a raw key
 * (msgpack array).
 */
static inline int
vy_entry_compare_with_raw_key(struct vy_entry entry,
			      const char *key, hint_t key_hint,
			      struct key_def *key_def)
{
	return vy_stmt_compare_with_raw_key(entry.stmt, entry.hint,
					    key, key_hint, key_def);
}

/**
 * The sign of a statement among equal keys.
 * @retval -1 an infimum key.
 * @retval 1 a supremum key.
 * @retval 0 a plain key or a tuple.
 */
static inline int
vy_bound_sign(struct tuple *stmt)
{
	uint16_t flags = vy_stmt_flags(stmt);
	if (flags & VY_STMT_INFIMUM)
		return -1;
	if (flags & VY_STMT_SUPREMUM)
		return 1;
	return 0;
}

/**
 * The bound a scan of the given iterator type starts from,
 * relative to the search key. A type that includes its key's
 * matches starts on the near side and travels through them: GE
 * at the key's infimum, LE at its supremum. An exclusive type
 * starts on the far side, its matches behind: GT at the
 * supremum, LT at the infimum.
 * @retval VY_STMT_SUPREMUM for GT, LE and REQ.
 * @retval VY_STMT_INFIMUM for all others.
 */
static inline uint16_t
vy_bound_flag(enum iterator_type type)
{
	const unsigned mask = (1u << ITER_GT) | (1u << ITER_LE) |
			      (1u << ITER_REQ);
	return (mask & (1u << type)) ? VY_STMT_SUPREMUM :
				       VY_STMT_INFIMUM;
}

/**
 * @return true if @a stmt is an infimum or supremum key, see
 * vy_bound_sign().
 */
static inline bool
vy_stmt_is_bound(struct tuple *stmt)
{
	return (vy_stmt_flags(stmt) &
		(VY_STMT_INFIMUM | VY_STMT_SUPREMUM)) != 0;
}

/**
 * Make the bound a scan of @a order starts from -- a private
 * copy of @a key marked with the order's bound flag, see
 * vy_bound_flag(). For keys the caller does not own; a freshly
 * created key is marked in place instead.
 * @param key the key, a plain statement.
 * @param order the scan order the bound positions.
 * @retval the bound entry, referenced.
 * @retval none allocation failed.
 */
static inline struct vy_entry
vy_bound_new(struct vy_entry key, enum iterator_type order)
{
	assert(!vy_stmt_is_bound(key.stmt));
	struct vy_entry bound;
	bound.stmt = vy_stmt_dup(key.stmt);
	if (bound.stmt == NULL)
		return vy_entry_none();
	bound.hint = key.hint;
	vy_stmt_add_flag(bound.stmt, vy_bound_flag(order));
	/*
	 * A bound key's main function is to mark the start or the
	 * end of a range, and in this role it could carry any
	 * LSN. But later a DELETE may be fused into it, and then
	 * the bound key's LSN stores the LSN of the fused DELETE.
	 * Set the LSN to 0 to indicate no fusion has taken place
	 * yet.
	 */
	vy_stmt_set_lsn(bound.stmt, 0);
	return bound;
}

/**
 * Check if @a entry is the bound of @a key on the side of
 * @a sign -- the key's own bound, not a wider key's, which
 * compares equal on the common prefix but covers a range of its
 * own: the part counts must match too.
 * @param entry the entry to check.
 * @param key the key whose bound is sought.
 * @param sign -1 for the infimum bound, 1 for the supremum.
 * @param cmp_def the comparison key definition.
 * @retval true @a entry is the bound.
 */
static inline bool
vy_entry_is_bound_of(struct vy_entry entry, struct vy_entry key, int sign,
		     struct key_def *cmp_def)
{
	return vy_stmt_is_bound(entry.stmt) &&
	       vy_bound_sign(entry.stmt) == sign &&
	       vy_stmt_key_part_count(entry.stmt, cmp_def) ==
	       vy_stmt_key_part_count(key.stmt, cmp_def) &&
	       vy_entry_compare(entry, key, cmp_def) == 0;
}

/**
 * Provide a total order for partial keys and keys with
 * INFIMUM/SUPREMUM bits, as (sign, part count) pairs of keys
 * sharing their common parts. The total order is necessary to
 * guarantee no tuple escapes a bound driven range scan in the
 * tuple cache: over an index on (a, b) it sorts, e.g.,
 *
 *   [10]- < [10] < {10, 1} < {10, 2} < [10, 2]+ < [10]+
 *
 * where [10]- and [10]+ are the infimum and supremum keys of 10
 * and [10] is a bare partial key. Every tuple with prefix 10
 * sorts strictly between the bound keys of 10, so a chain
 * bracketed by them provably covers the scan's whole match
 * range, and a bound key of a shorter prefix hugs the wider
 * range: [10]- sorts before [10, 2]-, [10, 2]+ before [10]+.
 * The comparison is decided at the first position where one key
 * has no part, and the missing part compares as the key's
 * padding: -inf for an infimum key, a shade above it for a bare
 * partial key, +inf for a supremum key.
 * @retval <0 the first key sorts before the second.
 * @retval 0 the keys share the position.
 * @retval >0 the first key sorts after the second.
 */
static inline int
vy_total_order_cmp(int sign_a, uint32_t parts_a, int sign_b,
		   uint32_t parts_b)
{
	if (parts_a != parts_b) {
		/*
		 * The shorter key's padding against the longer key's
		 * real part: below it unless the pad is +inf.
		 */
		if (parts_a < parts_b)
			return sign_a > 0 ? 1 : -1;
		return sign_b > 0 ? -1 : 1;
	}
	/* Pads meet pads at the same position: -inf < bare < +inf. */
	return sign_a - sign_b;
}

/**
 * Compare two entries as positions in the total key order: equal
 * common parts are tie-broken by the keys' padding signs and part
 * counts, see vy_total_order_cmp().
 * @retval <0 @a a sorts before @a b.
 * @retval 0 @a a and @a b share the position.
 * @retval >0 @a a sorts after @a b.
 */
static inline int
vy_bound_cmp(struct vy_entry a, struct vy_entry b,
	     struct key_def *cmp_def)
{
	int rc = vy_entry_compare(a, b, cmp_def);
	if (rc != 0)
		return rc;
	return vy_total_order_cmp(vy_bound_sign(a.stmt),
				  vy_stmt_key_part_count(a.stmt, cmp_def),
				  vy_bound_sign(b.stmt),
				  vy_stmt_key_part_count(b.stmt, cmp_def));
}

/**
 * Iterate over each key indexed in the given statement.
 * @param entry    loop variable
 * @param src_stmt source statement
 * @param key_def  key definition
 *
 * For a multikey index, entry.hint is set to multikey entry offset
 * and the loop iterates over each offset stored in the statement.
 *
 * For a unikey index, entry.hint is initialized with vy_stmt_hint()
 * and the loop breaks after the first iteration.
 *
 * entry.stmt is set to src_stmt on each iteration.
 */
#define vy_stmt_foreach_entry(entry, src_stmt, key_def)			\
	for (uint32_t multikey_idx = 0,					\
	     multikey_count = !(key_def)->is_multikey ? 1 :		\
			tuple_multikey_count((src_stmt), (key_def));	\
	     multikey_idx < multikey_count &&				\
	     (((entry).stmt = (src_stmt)),				\
	      ((entry).hint = !(key_def)->is_multikey ?			\
			vy_stmt_hint((src_stmt), (key_def)) :		\
			multikey_idx), true);				\
	     ++multikey_idx)						\
		if (!tuple_key_is_excluded(src_stmt, key_def,		\
					   multikey_idx))

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_STMT_H */
