#pragma once
/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2026, Tarantool AUTHORS, please see AUTHORS file.
 *
 * LCP (Longest Common Prefix) compression for sorted string arrays.
 *
 * Strings are divided into fixed-size groups. Each group stores the
 * full first string (the "restart point") and, for every string in
 * the group, only the suffix after stripping the group-wide LCP.
 *
 * Lookup is two-level: binary search over group boundary keys, then
 * short linear scan within the group, reconstructing each key by
 * concatenating the prefix and the suffix.
 *
 * Uses blocked front coding (a.k.a. "head compression" in Binna
 * et al., "Revisiting Prefix and Suffix Compressed B+-Trees",
 * SIGMOD 2024).
 *
 * This header is a generic, self-contained data structure. It knows
 * nothing about vinyl, pages, msgpack keys, or comparators. The
 * caller is responsible for key comparison and index mapping.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "msgpuck/msgpuck.h"
#include "trivia/util.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum {
	/** Maximum number of strings per group. */
	LCP_GROUP_MAX = 16,
	/** Initial capacity of the group builder's key buffer. */
	LCP_GROUP_BUF_INIT = 500,
};

/**
 * A group of sorted strings compressed with a shared LCP.
 *
 * All metadata is packed into @a data as leading msgpack values,
 * decoded on access by lcp_group_iter_init().
 */
struct lcp_group {
	/**
	 * Full boundary string (first string in the group),
	 * followed by packed group data in the same
	 * allocation. Freed by lcp_group_destroy().
	 *
	 * Packed data layout (starts at mp_next(key)):
	 *   mp_uint  prefix_len
	 *   mp_uint  data_len (total length of the group's
	 *            allocation, from @a key through the last
	 *            mp_str; used as the iteration terminator)
	 *   mp_str   suffix[0]
	 *   mp_str   suffix[1]
	 *   ...
	 *   mp_str   suffix[N - 1]
	 *
	 * The number of suffixes is not stored -- iteration
	 * terminates when the current position reaches
	 * @a key + data_len. The builder uses LCP_GROUP_MAX for
	 * all groups except the last; the last group's size is
	 * implicit in its data_len.
	 */
	const char *key;
};

/**
 * Iterator for linear scan within a group.
 *
 * Initialized by lcp_group_iter_init(), which unpacks the
 * metadata header from the group. Then the caller iterates
 * with lcp_group_iter_next_key().
 */
struct lcp_group_iter {
	/** Pointer to the current suffix in data. */
	const char *cur;
	/** End of data; iteration stops when @a cur reaches it. */
	const char *data_end;
	/**
	 * Caller-provided buffer for key reconstruction.
	 * Prefix is copied here at init time; suffixes are
	 * appended at buf + prefix_len on each next_key call.
	 * Must be at least prefix_len + max_suffix_len bytes.
	 */
	char *buf;
	/** Length of the shared prefix. */
	uint32_t prefix_len;
};

/**
 * Compute the byte-level longest common prefix of two byte strings.
 * Returns the number of common leading bytes.
 */
static inline uint32_t
lcp_len(const char *a, uint32_t a_len, const char *b, uint32_t b_len)
{
	uint32_t min_len = MIN(a_len, b_len);
	uint32_t i = 0;
	while (i + 8 <= min_len && memcmp(a + i, b + i, 8) == 0)
		i += 8;
	while (i < min_len && a[i] == b[i])
		i++;
	return i;
}

/** Free the memory owned by a group (key + packed data). */
static inline void
lcp_group_destroy(struct lcp_group *group)
{
	free((char *)group->key);
}

/* LCP group iterator. {{{ */

/** Initialize an iterator over a group. */
static inline void
lcp_group_iter_init(struct lcp_group_iter *it,
		    const struct lcp_group *group, char *buf)
{
	const char *pos = group->key;
	it->buf = buf;
	mp_next(&pos);
	it->prefix_len = mp_decode_uint(&pos);
	/* Copy the shared prefix once; suffixes are appended. */
	memcpy(it->buf, group->key, it->prefix_len);
	uint32_t data_len = mp_decode_uint(&pos);
	it->cur = pos;
	it->data_end = group->key + data_len;
}

/**
 * Advance to the next entry and reconstruct the full key
 * in the buffer passed to lcp_group_iter_init(). The prefix
 * is already in buf from init; only the suffix is copied.
 *
 * Iteration terminates when the current position reaches
 * the data_end marker.
 *
 * @param[out] len  Reconstructed key length.
 * @return Pointer to the iterator's buffer, or NULL if
 *         exhausted.
 */
static inline const char *
lcp_group_iter_next_key(struct lcp_group_iter *it, uint32_t *len)
{
	if (it->cur >= it->data_end)
		return NULL;
	uint32_t suffix_len;
	const char *suffix = mp_decode_str(&it->cur, &suffix_len);
	memcpy(it->buf + it->prefix_len, suffix, suffix_len);
	*len = it->prefix_len + suffix_len;
	return it->buf;
}

/* }}} LCP group iterator */

/* LCP index: array of groups with metadata. {{{ */

/** Array of LCP groups with element count and memory accounting. */
struct lcp_index {
	/** Dense array of @a count groups, allocated in one chunk. */
	struct lcp_group *groups;
	/** Number of groups currently stored in @a groups. */
	uint32_t count;
	/**
	 * Total memory used by this index: groups array +
	 * all group key/data allocations.
	 */
	size_t mem_used;
	/**
	 * Max reconstructed key length across all groups.
	 * Lets the caller allocate one buffer large enough
	 * for any key in the index.
	 */
	uint32_t max_key_len;
	/**
	 * Sum of prefix_len across all groups. Divided by
	 * group count gives the average LCP -- a diagnostic
	 * measure of how effective the compression is for
	 * this index's key distribution.
	 */
	uint64_t total_prefix_len;
};

static inline void
lcp_index_create(struct lcp_index *index)
{
	index->groups = NULL;
	index->count = 0;
	index->mem_used = 0;
	index->max_key_len = 0;
	index->total_prefix_len = 0;
}

/** Destroy all groups and free the groups array. */
static inline void
lcp_index_destroy(struct lcp_index *index)
{
	for (uint32_t i = 0; i < index->count; i++)
		lcp_group_destroy(&index->groups[i]);
	free(index->groups);
	TRASH(index);
}

/**
 * Return the boundary key (first key) of the group that
 * contains the given element index.
 */
static inline const char *
lcp_index_group_key(const struct lcp_index *index, uint32_t elem_no)
{
	uint32_t grp = elem_no / LCP_GROUP_MAX;
	assert(grp < index->count);
	return index->groups[grp].key;
}

/* }}} LCP index */

/* LCP index sequential iterator. {{{ */

/**
 * Sequential iterator over all entries in an lcp_index.
 * Walks groups in order, advancing to the next group
 * automatically.
 *
 * Usage:
 *   struct lcp_index_iter it;
 *   lcp_index_iter_init(&it, &index, buf);
 *   uint32_t key_len;
 *   const char *key;
 *   while ((key = lcp_index_iter_next_key(&it, &key_len)) != NULL) {
 *       // key points to buf with the reconstructed key
 *   }
 */
struct lcp_index_iter {
	/** The index being iterated. */
	const struct lcp_index *index;
	/** Key reconstruction buffer, caller-provided. */
	char *buf;
	/** Current group number. */
	uint32_t group_no;
	/** Iterator within the current group. */
	struct lcp_group_iter group_iter;
};

static inline void
lcp_index_iter_init(struct lcp_index_iter *it,
		    const struct lcp_index *index, char *buf)
{
	it->index = index;
	it->buf = buf;
	it->group_no = 0;
	if (index->count > 0) {
		lcp_group_iter_init(&it->group_iter,
				    &index->groups[0], buf);
	}
}

/**
 * Advance to the next entry and reconstruct the full key
 * in the buffer passed to lcp_index_iter_init(). The prefix
 * is copied only when advancing to a new group; each call
 * only copies the suffix.
 *
 * @param[out] len  Reconstructed key length.
 * @return Pointer to the iterator's buffer, or NULL if
 *         exhausted.
 */
static inline const char *
lcp_index_iter_next_key(struct lcp_index_iter *it, uint32_t *len)
{
	if (it->group_no >= it->index->count)
		return NULL;
	const char *key = lcp_group_iter_next_key(&it->group_iter, len);
	if (key != NULL)
		return key;
	/* Group exhausted, advance to the next one. */
	it->group_no++;
	if (it->group_no >= it->index->count)
		return NULL;
	lcp_group_iter_init(&it->group_iter,
			    &it->index->groups[it->group_no],
			    it->buf);
	return lcp_group_iter_next_key(&it->group_iter, len);
}

/* }}} LCP index sequential iterator */

/* LCP group builder (internal helper). {{{ */

/**
 * Accumulates keys for a single group. Copies key data into
 * an internal buffer that is reused across groups.
 */
struct lcp_group_builder {
	/** Offsets of each key in @a buf. */
	uint32_t key_offsets[LCP_GROUP_MAX];
	/** Length of each key in @a buf. */
	uint32_t key_lens[LCP_GROUP_MAX];
	/** Number of keys accumulated so far. */
	uint32_t key_count;
	/** Contiguous buffer holding copies of all keys. */
	char *buf;
	/** Bytes currently used in @a buf. */
	uint32_t buf_used;
	/** Allocated size of @a buf, in bytes. */
	uint32_t buf_cap;
};

static inline void
lcp_group_builder_init(struct lcp_group_builder *b)
{
	b->key_count = 0;
	b->buf = NULL;
	b->buf_used = 0;
	b->buf_cap = 0;
}

/** Clear accumulated keys so the buffer can be reused. */
static inline void
lcp_group_builder_reset(struct lcp_group_builder *b)
{
	b->key_count = 0;
	b->buf_used = 0;
}

/**
 * Ensure the builder's buffer has room for @a needed more bytes
 * on top of what's already used.
 * @retval  0 success
 * @retval -1 allocation failure
 */
static inline int
lcp_group_builder_ensure(struct lcp_group_builder *b, uint32_t needed)
{
	uint32_t min_cap = b->buf_used + needed;
	if (min_cap <= b->buf_cap)
		return 0;
	uint32_t new_cap = b->buf_cap > 0 ? b->buf_cap * 2 :
			   LCP_GROUP_BUF_INIT;
	while (new_cap < min_cap)
		new_cap *= 2;
	char *new_buf = (char *)realloc(b->buf, new_cap);
	if (new_buf == NULL)
		return -1;
	b->buf = new_buf;
	b->buf_cap = new_cap;
	return 0;
}

/**
 * Copy @a key into the builder's internal buffer.
 * @retval  0 success
 * @retval -1 allocation failure
 */
static inline int
lcp_group_builder_add(struct lcp_group_builder *b,
		      const char *key, uint32_t key_len)
{
	assert(b->key_count < LCP_GROUP_MAX);
	if (lcp_group_builder_ensure(b, key_len) != 0)
		return -1;
	memcpy(b->buf + b->buf_used, key, key_len);
	b->key_offsets[b->key_count] = b->buf_used;
	b->key_lens[b->key_count] = key_len;
	b->buf_used += key_len;
	b->key_count++;
	return 0;
}

/**
 * Finish building a group. Computes the LCP of the first and last
 * key (which bounds the LCP of all keys since they are sorted),
 * allocates group->key (with packed data appended) and packs
 * the metadata and suffixes.
 *
 * @param[out] mem_allocated  Total bytes malloc'd (key + data).
 * @param[out] out_prefix_len LCP length in bytes.
 * @param[out] out_max_key_len  Max reconstructed key length in
 *                              this group (prefix_len + longest
 *                              suffix).
 * @retval  0 success
 * @retval -1 memory allocation failure
 */
static inline int
lcp_group_builder_finish(struct lcp_group_builder *b,
			 struct lcp_group *group,
			 size_t *mem_allocated,
			 uint32_t *out_prefix_len,
			 uint32_t *out_max_key_len)
{
	assert(b->key_count > 0);

	/* Compute LCP from first and last key. */
	const char *first_key = b->buf + b->key_offsets[0];
	const char *last_key = b->buf +
			       b->key_offsets[b->key_count - 1];
	uint32_t prefix_len = lcp_len(first_key, b->key_lens[0],
				      last_key,
				      b->key_lens[b->key_count - 1]);

	/*
	 * Calculate data buffer length:
	 *   meta: prefix_len + data_len (mp_uint)
	 *   body: sum of mp_sizeof_str(suffix_len) for each key
	 */
	uint32_t max_suffix_len = 0;
	uint32_t body_len = 0;
	for (uint32_t i = 0; i < b->key_count; i++) {
		uint32_t suffix_len = b->key_lens[i] - prefix_len;
		if (suffix_len > max_suffix_len)
			max_suffix_len = suffix_len;
		body_len += mp_sizeof_str(suffix_len);
	}
	/*
	 * data_len is the total length of the group's
	 * allocation, from group->key through the last suffix.
	 * The meta header length depends on the mp_uint encoding
	 * of data_len, which itself depends on the meta header
	 * length, so iterate to a fixed point.
	 */
	uint32_t key_len = b->key_lens[0];
	uint32_t data_len = key_len + body_len;
	uint32_t meta_len, meta_newlen;
	do {
		meta_len = mp_sizeof_uint(prefix_len) +
			   mp_sizeof_uint(data_len);
		data_len = key_len + meta_len + body_len;
		meta_newlen = mp_sizeof_uint(prefix_len) +
			      mp_sizeof_uint(data_len);
	} while (meta_len != meta_newlen);

	char *buf = (char *)malloc(data_len);
	if (buf == NULL)
		return -1;
	memcpy(buf, first_key, key_len);
	group->key = buf;

	/* Encode meta header. */
	char *pos = buf + key_len;
	pos = mp_encode_uint(pos, prefix_len);
	pos = mp_encode_uint(pos, data_len);
	assert((uint32_t)(pos - (buf + key_len)) == meta_len);

	/* Encode suffixes. */
	for (uint32_t i = 0; i < b->key_count; i++) {
		const char *key_i = b->buf + b->key_offsets[i];
		uint32_t suffix_len = b->key_lens[i] - prefix_len;
		pos = mp_encode_str(pos, key_i + prefix_len,
				    suffix_len);
	}
	assert((uint32_t)(pos - buf) == data_len);
	*mem_allocated = data_len;
	*out_prefix_len = prefix_len;
	*out_max_key_len = prefix_len + max_suffix_len;
	return 0;
}

/** Free the internal buffer. */
static inline void
lcp_group_builder_destroy(struct lcp_group_builder *b)
{
	free(b->buf);
}

/* }}} LCP group builder */

/* LCP builder: incrementally builds an lcp_index. {{{ */

/**
 * Builder for constructing an lcp_index incrementally.
 * Holds a reference to the target index and automatically
 * flushes full groups.
 *
 * Usage:
 *   lcp_builder_init(&b, &index);
 *   lcp_builder_reserve(&b, page_count);
 *   for each string:
 *       lcp_builder_add(&b, str, str_len);
 *   int rc = lcp_builder_finish(&b);
 *   lcp_builder_destroy(&b);
 *
 * Keys are copied into an internal buffer that is reused
 * across groups and freed by lcp_builder_destroy(). The
 * caller retains ownership of the key pointers passed to
 * add().
 *
 * Allocation failures in reserve() and add() are deferred:
 * the builder is marked as failed and all subsequent add()
 * calls are no-ops. The error is reported by finish(),
 * which the caller must always check. destroy() must be
 * called regardless of success or failure.
 */
struct lcp_builder {
	/** Target index. */
	struct lcp_index *index;
	/**
	 * Allocated capacity of index->groups during build.
	 * On finish, the array is shrunk to index->count and
	 * the builder is destroyed.
	 */
	uint32_t capacity;
	/** Per-group accumulator. */
	struct lcp_group_builder group;
	/** True if an allocation failed during reserve/add. */
	bool failed;
};

static inline void
lcp_builder_init(struct lcp_builder *b, struct lcp_index *index)
{
	b->index = index;
	b->capacity = 0;
	b->failed = false;
	lcp_group_builder_init(&b->group);
}

/**
 * Pre-allocate capacity for at least @a page_count elements
 * (rounded up to groups of LCP_GROUP_MAX). On failure the
 * builder is marked as failed; the error is reported by
 * lcp_builder_finish().
 */
static inline void
lcp_builder_reserve(struct lcp_builder *b, uint32_t page_count)
{
	uint32_t cap = (page_count + LCP_GROUP_MAX - 1) /
		       LCP_GROUP_MAX;
	if (cap <= b->capacity)
		return;
	struct lcp_index *index = b->index;
	struct lcp_group *g = (struct lcp_group *)
		calloc(cap, sizeof(*g));
	if (g == NULL) {
		b->failed = true;
		return;
	}
	if (index->groups != NULL) {
		memcpy(g, index->groups,
		       index->count * sizeof(*g));
		free(index->groups);
	}
	index->groups = g;
	b->capacity = cap;
}

/**
 * Ensure the groups array has room for one more group.
 * @retval  0 success
 * @retval -1 allocation failure
 */
static inline int
lcp_builder_ensure(struct lcp_builder *b)
{
	struct lcp_index *index = b->index;
	if (index->count < b->capacity)
		return 0;
	uint32_t new_cap = b->capacity > 0 ? b->capacity * 2 : 16;
	struct lcp_group *g = (struct lcp_group *)
		realloc(index->groups, new_cap * sizeof(*g));
	if (g == NULL)
		return -1;
	index->groups = g;
	b->capacity = new_cap;
	return 0;
}

/**
 * Finish the current per-group accumulator and append the
 * resulting group to the index. Does nothing if empty.
 * @retval  0 success
 * @retval -1 allocation failure
 */
static inline int
lcp_builder_flush(struct lcp_builder *b)
{
	if (b->group.key_count == 0)
		return 0;
	if (lcp_builder_ensure(b) != 0)
		return -1;
	struct lcp_index *index = b->index;
	struct lcp_group *g = &index->groups[index->count];
	size_t mem_allocated;
	uint32_t g_prefix_len;
	uint32_t g_max_key_len;
	if (lcp_group_builder_finish(&b->group, g, &mem_allocated,
				     &g_prefix_len,
				     &g_max_key_len) != 0)
		return -1;
	index->mem_used += mem_allocated;
	if (g_max_key_len > index->max_key_len)
		index->max_key_len = g_max_key_len;
	index->total_prefix_len += g_prefix_len;
	index->count++;
	lcp_group_builder_reset(&b->group);
	return 0;
}

/**
 * Copy @a key into the builder. The key is borrowed -- the
 * builder makes its own copy into an internal buffer that is
 * reused across groups.
 *
 * On allocation failure the builder is marked as failed;
 * subsequent calls are no-ops. The error is reported by
 * lcp_builder_finish().
 */
static inline void
lcp_builder_add(struct lcp_builder *b,
		const char *key, uint32_t key_len)
{
	if (b->group.key_count == LCP_GROUP_MAX) {
		if (lcp_builder_flush(b) != 0) {
			b->failed = true;
			return;
		}
	}
	if (lcp_group_builder_add(&b->group, key, key_len) != 0)
		b->failed = true;
}

/**
 * Like lcp_builder_add, but the key is a raw msgpack value.
 * The length is computed via mp_next.
 */
static inline void
lcp_builder_add_mp(struct lcp_builder *b, const char *key)
{
	const char *key_end = key;
	mp_next(&key_end);
	lcp_builder_add(b, key, key_end - key);
}

/**
 * Finish building: flush the last partial group and shrink
 * the index to fit. The caller must always call
 * lcp_builder_destroy() afterwards to free the scratch
 * buffer, regardless of success or failure.
 *
 * @retval  0 success
 * @retval -1 allocation failure (possibly deferred from add)
 */
static inline int
lcp_builder_finish(struct lcp_builder *b)
{
	if (b->failed || lcp_builder_flush(b) != 0)
		return -1;
	/* Shrink the groups array to fit exactly count entries. */
	struct lcp_index *index = b->index;
	if (index->count > 0 && b->capacity > index->count) {
		struct lcp_group *g = (struct lcp_group *)
			realloc(index->groups,
				index->count * sizeof(*g));
		if (g != NULL) {
			index->groups = g;
			b->capacity = index->count;
		}
	}
	index->mem_used += index->count * sizeof(*index->groups);
	return 0;
}

/**
 * Free the builder's scratch buffer. Must be called exactly
 * once per lcp_builder_init, regardless of whether finish
 * succeeded or failed.
 */
static inline void
lcp_builder_destroy(struct lcp_builder *b)
{
	lcp_group_builder_destroy(&b->group);
}

/* }}} LCP builder */

#if defined(__cplusplus)
}
#endif
