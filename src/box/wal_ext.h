/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2022, Tarantool AUTHORS, please see AUTHORS file.
 */
#pragma once

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include <stddef.h>
#include <stdbool.h>
#include <lua.h>

/** Initialize WAL extensions cache. */
void
wal_ext_init(void);

/** Cleanup extensions cache and default value. */
void
wal_ext_free(void);

/** The set of WAL extensions enabled by box.cfg.wal_ext. */
struct wal_extensions_config {
	/** Append the old and new tuples to journaled rows. */
	bool new_old;
};

/** Parse WAL extensions config from lua value */
int
luaT_wal_ext_config_create(struct lua_State *L, int idx,
			   struct wal_extensions_config *ext_config);

/** Load WAL extensions configuration. */
void
wal_ext_set_cfg(struct wal_extensions_config *ext_config);

/** True if any WAL extension is currently enabled. */
bool
wal_ext_is_enabled(void);

struct space_wal_ext;
struct txn_stmt;
struct request;

/**
 * Fills in @a request with data from @a stmt depending on space's WAL
 * extensions.
 */
void
space_wal_ext_process_request(struct space_wal_ext *ext, struct txn_stmt *stmt,
			      struct request *request);

/**
 * Return the WAL extension to attach to a space, NULL if none
 * is enabled. The returned object MUST NOT be freed or changed
 * in any way; it should be read-only.
 */
struct space_wal_ext *
wal_ext(void);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */
