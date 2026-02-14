#ifndef TARANTOOL_LIB_SALAD_MINHASH_H_INCLUDED
#define TARANTOOL_LIB_SALAD_MINHASH_H_INCLUDED
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
 * MinHash sketch for estimating Jaccard similarity between sets.
 *
 * The sketch consists of MINHASH_K minimum hash values, one per
 * independent hash function.  Each "hash function" is implemented
 * as murmur64(element_hash + seed_i), where seed_i are fixed
 * constants.
 *
 * Two sketches can be merged by taking the element-wise minimum.
 * The Jaccard similarity is estimated as the fraction of positions
 * where both sketches agree.
 */

#include <stdint.h>
#include <string.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** Number of hash functions in the MinHash sketch. */
enum { MINHASH_K = 64 };

/** Size of a serialized MinHash sketch in bytes. */
enum { MINHASH_SIZE = MINHASH_K * sizeof(uint64_t) };

/**
 * A MinHash sketch.
 */
struct minhash {
	uint64_t values[MINHASH_K];
};

/**
 * Murmur64 finalizer (same as in binaryfusefilter.h).
 */
static inline uint64_t
minhash_murmur64(uint64_t h)
{
	h ^= h >> 33U;
	h *= UINT64_C(0xff51afd7ed558ccd);
	h ^= h >> 33U;
	h *= UINT64_C(0xc4ceb9fe1a85ec53);
	h ^= h >> 33U;
	return h;
}

/**
 * Fixed seeds for the MINHASH_K hash functions.
 * Generated from a splitmix64 sequence starting at an arbitrary
 * constant.
 */
static inline uint64_t
minhash_seed(uint32_t i)
{
	/*
	 * splitmix64 with initial state 0x9E3779B97F4A7C15 + i.
	 * This gives a deterministic, well-distributed seed per slot.
	 */
	uint64_t z = UINT64_C(0x9E3779B97F4A7C15) + (uint64_t)i;
	z = (z ^ (z >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27U)) * UINT64_C(0x94D049BB133111EB);
	return z ^ (z >> 31U);
}

/**
 * Initialize a MinHash sketch to "empty" state.
 * All values are set to UINT64_MAX.
 */
static inline void
minhash_create(struct minhash *sketch)
{
	memset(sketch->values, 0xff, sizeof(sketch->values));
}

/**
 * Add an element (given by its 64-bit hash) to the sketch.
 */
static inline void
minhash_add(struct minhash *sketch, uint64_t hash)
{
	for (uint32_t i = 0; i < MINHASH_K; i++) {
		uint64_t val = minhash_murmur64(hash + minhash_seed(i));
		if (val < sketch->values[i])
			sketch->values[i] = val;
	}
}

/**
 * Merge sketch @a src into @a dst (element-wise minimum).
 * This is equivalent to the union of the underlying sets.
 */
static inline void
minhash_merge(struct minhash *dst, const struct minhash *src)
{
	for (uint32_t i = 0; i < MINHASH_K; i++) {
		if (src->values[i] < dst->values[i])
			dst->values[i] = src->values[i];
	}
}

/**
 * Estimate Jaccard similarity J(A, B) between two sketches.
 *
 * Returns a value in [0, 1]:
 *   0 = no overlap (disjoint sets)
 *   1 = identical sets
 *
 * @return Estimated Jaccard similarity.
 */
static inline double
minhash_jaccard(const struct minhash *a, const struct minhash *b)
{
	uint32_t matching = 0;
	for (uint32_t i = 0; i < MINHASH_K; i++) {
		if (a->values[i] == b->values[i])
			matching++;
	}
	return (double)matching / MINHASH_K;
}

/**
 * Check if a sketch is empty (no elements added).
 */
static inline int
minhash_is_empty(const struct minhash *sketch)
{
	return sketch->values[0] == UINT64_MAX;
}

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_LIB_SALAD_MINHASH_H_INCLUDED */
