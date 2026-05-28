#include "box/wal_ext_impl.h"
#include "stdio.h"
#include "trivia/util.h"
#include "tuple.h"
#include "xrow.h"
#include "txn.h"

/*
 * space_wal_ext is a virtual table with single method that process
 * a wal request.
 */
struct space_wal_ext {
	/* Process an WAL request. */
	void (*process)(struct txn_stmt *stmt, struct request *request);
};

static struct wal_extensions_config global_extensions = {
	.new_old = false,
};

void
append_old_and_new_tuple(struct txn_stmt *stmt, struct request *request)
{
	if (stmt->new_tuple != NULL &&
	    (request->type == IPROTO_INSERT ||
	     request->type == IPROTO_UPDATE ||
	     request->type == IPROTO_UPSERT ||
	     request->type == IPROTO_REPLACE)) {
		const char *new_data = tuple_data(stmt->new_tuple);
		request->new_tuple = new_data;
		request->new_tuple_end =
			new_data + tuple_bsize(stmt->new_tuple);
	}

	if (stmt->old_tuple != NULL &&
	    (request->type == IPROTO_DELETE ||
	     request->type == IPROTO_UPDATE ||
	     request->type == IPROTO_UPSERT ||
	     request->type == IPROTO_REPLACE)) {
		const char *old_data = tuple_data(stmt->old_tuple);
		request->old_tuple = old_data;
		request->old_tuple_end =
			old_data + tuple_bsize(stmt->old_tuple);
	}
}

/*
 * new_old extension
 */
static struct space_wal_ext new_old_ext = {
	.process = append_old_and_new_tuple,
};

void
wal_ext_init(void)
{
}

void
wal_ext_free(void)
{
}

int
luaT_wal_ext_config_create(struct lua_State *L, int idx,
			   struct wal_extensions_config *ext_config)
{
	memset(ext_config, 0, sizeof(struct wal_extensions_config));

	if (idx < 0)
		idx = lua_gettop(L) + idx + 1;
	assert(idx > 0);

	if (lua_isnil(L, idx))
		return 0;

	bool is_table = lua_istable(L, idx);
	if (!is_table) {
		diag_set(IllegalParams,
			 "`wal_ext` value must be a table or nil");
		return -1;
	}

	/* parse the wal_ext table as a set of
	 *     {[extension] = [enabled]} pairs
	 */
	lua_pushnil(L);
	/* "wal_ext" table now at -2 */
	while (lua_next(L, idx) != 0) {
		const char *ext_name = lua_tostring(L, -2); /* key */

		if (!lua_isboolean(L, -1)) {
			diag_set(IllegalParams,
				 "value for extension `%s` must be a boolean",
				 ext_name);
			lua_pop(L, 2); /* pop `enable` and `key` */
			return -1;
		}
		bool enable = lua_toboolean(L, -1); /* value */

		if (strcmp(ext_name, "new_old") == 0) {
			ext_config->new_old = enable;
		} else {
			diag_set(IllegalParams,
				 "extension `%s` does not exist",
				 ext_name);
			lua_pop(L, 2); /* pop `enable` and `key` */
			return -1;
		}

		/* pop `enable` */
		lua_pop(L, 1);
	}

	/* lua_next leaves a clean stack when iteration finishes */

	return 0;
}

bool
wal_ext_is_enabled(void)
{
	return global_extensions.new_old;
}

void
wal_ext_set_cfg(struct wal_extensions_config *ext_config)
{
	if (ext_config->new_old != global_extensions.new_old) {
		say_info("%s new_old WAL extension",
			 ext_config->new_old ? "Enabling" : "Disabling");
	}
	global_extensions = *ext_config;
}

void
space_wal_ext_process_request(struct space_wal_ext *ext, struct txn_stmt *stmt,
			      struct request *request)
{
	assert(ext != NULL);
	ext->process(stmt, request);
}

struct space_wal_ext *
space_wal_ext_by_name(const char *space_name)
{
	(void)space_name;
	if (global_extensions.new_old) {
		return &new_old_ext;
	}
	return NULL;
}
