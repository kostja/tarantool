#pragma once

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#include <stddef.h>
#include <lua.h>
#include <stdbool.h>

/** Initialize WAL extensions cache. */
void
wal_ext_init(void);

/** Cleanup extensions cache and default value. */
void
wal_ext_free(void);

struct wal_extensions_config {
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
 * Return reference to corresponding WAL extension by given space name.
 * Returned object MUST NOT be freed or changed in any way; it should be
 * read-only.
 */
struct space_wal_ext *
space_wal_ext_by_name(const char *space_name);

/**
 * Globally enabled or disabled an extension. Return -1 if there is no
 * such extension.
 * @param ext_name name of extension
 * @param enable true if extension enabled, false otherwise
 */
int
set_enable_extension(const char *ext_name, bool enable);
#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */
