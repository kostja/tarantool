#ifndef INCLUDES_TARANTOOL_BOX_VY_POINT_LOOKUP_H
#define INCLUDES_TARANTOOL_BOX_VY_POINT_LOOKUP_H
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

/**
 * Point lookup is a special case of read iterator that is designed for
 * retrieving one value from an LSM tree by a full key (all parts are present).
 *
 * Iterator collects necessary history of the given key from different sources
 * (txw, cache, mems, runs) that consists of some number of sequential upserts
 * and possibly one terminal statement (replace or delete). The iterator
 * sequentially scans txw, cache, mems and runs until a terminal statement is
 * met. After reading the slices the iterator checks that the list of mems
 * hasn't been changed and restarts if it is the case.
 * After the history is collected the iterator calculates resultant statement
 * and, if the result is the latest version of the key, adds it to cache.
 */

#include "vy_entry.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_lsm;
struct vy_tx;
struct vy_read_view;

/**
 * Given a key that has all index parts (including primary index
 * parts in case of a secondary index), lookup the corresponding
 * tuple in the LSM tree. The tuple is returned in @ret with its
 * reference counter elevated.
 *
 * If @keep_delete flag is set, the function will return the latest
 * DELETE statement instead of NULL if it exists.
 *
 * If the mem or disk scan skipped any statement because its LSN
 * exceeds the read view, the result statement will have the
 * VY_STMT_STALE flag set. This tells the caller that the result
 * may differ from the global (latest) result and must not be
 * cached.
 *
 * Note, this function doesn't track the result in the transaction
 * read set, i.e. it is up to the caller to call vy_tx_track() if
 * necessary.
 */
int
vy_point_lookup(struct vy_lsm *lsm, struct vy_tx *tx,
		const struct vy_read_view **rv,
		struct vy_entry key, bool keep_delete,
		struct vy_entry *ret);

/**
 * Look up a tuple by key in memory.
 *
 * This function works just like vy_point_lookup() except:
 *
 * - It only scans in-memory level and cache and hence doesn't yield.
 * - It doesn't turn DELETE into NULL so it returns NULL if and only
 *   if no terminal statement matching the key is present in memory
 *   (there still may be statements stored on disk though).
 * - It doesn't account the lookup to LSM tree stats (as it never
 *   descends to lower levels).
 *
 * The function returns 0 on success, -1 on memory allocation error.
 */
int
vy_point_lookup_mem(struct vy_lsm *lsm, const struct vy_read_view **rv,
		    struct vy_entry key, struct vy_entry *ret);

/**
 * DML-time write-write conflict check for a snapshot TX. Checks
 * the PK (index_id == 0) or a unique SK for a concurrent write to
 * the same key as @a entry. For a unique SK the SK key is
 * extracted (NULLs skipped). Sealed mems, cache, and disk are
 * scanned for a version with LSN > the TX's vlsn. May yield on
 * disk reads.
 *
 * @param lsm   LSM tree to check (PK or unique SK).
 * @param tx    Snapshot TX performing the write.
 * @param entry Statement being written.
 *
 * @retval  0 No conflict.
 * @retval -1 Conflict found, or memory/read error (diag set).
 */
int
vy_lsm_check_concurrent_write(struct vy_lsm *lsm, struct vy_tx *tx,
			      struct vy_entry entry);

/**
 * Get a full tuple by a tuple read from a secondary index.
 * A tuple that is already the newest committed version of its
 * row is returned as is, without a primary lookup.
 * @param lsm         LSM tree from which the tuple was read.
 * @param tx          Current transaction.
 * @param rv          Read view.
 * @param entry       Tuple read from a secondary index.
 * @param[out] result The found tuple is stored here. Must be
 *                    unreferenced after usage.
 * @param[out] delete_lsn On a proven-dead key (a NULL @a result),
 *                         the LSN the key's deferred DELETE
 *                         inherits from the shadowing primary
 *                         statement, 0 if unknown. The caller
 *                         owns the cache consequences: report
 *                         the dead key to its chain or evict it.
 *
 * @retval  0 Success.
 * @retval -1 Memory error or read error.
 */
int
vy_get_by_secondary_tuple(struct vy_lsm *lsm, struct vy_tx *tx,
			  const struct vy_read_view **rv,
			  struct vy_entry entry, struct vy_entry *result,
			  int64_t *delete_lsn);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_POINT_LOOKUP_H */
