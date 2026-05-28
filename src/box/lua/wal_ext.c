#include "trivia/config.h"
#include "lua.h"
#include "luajit/src/lauxlib.h"
#include "lua/utils.h"
#include <cfg.h>
#include "box/box.h"
#include "box/lua/misc.h"
#include "box/wal_ext_impl.h"
#include "box/space_cache.h"
#include "say.h"

/**
 * Apply a dynamic change to box.cfg.wal_ext. Called only on
 * runtime reconfigure -- the initial box.cfg{} is intentionally
 * skipped via dynamic_cfg_skip_at_load, because the boot path
 * in main.cc already plumbs the initial value into
 * wal_ext_set_cfg() and no existing spaces are around to need
 * refresh.
 *
 * On a false -> true transition we also trigger a checkpoint
 * so that downstream CDC consumers have a clean bootstrap
 * point: every WAL row after the checkpoint is guaranteed to
 * carry the freshly-enabled extension, and any in-flight
 * transactions whose blind-write fast path skipped reading
 * the old tuple are already captured in the checkpoint's row
 * data (so the consumer doesn't need their old-tuple WAL row).
 */
static int
cfg_set_wal_ext(struct lua_State *L)
{
	struct wal_extensions_config new_cfg;
	if (cfg_get_wal_ext("wal_ext", &new_cfg) != 0)
		return luaT_error(L);

	bool was_on = wal_ext_is_enabled();
	bool now_on = new_cfg.new_old;

	wal_ext_set_cfg(&new_cfg);
	space_cache_refresh_wal_ext();

	if (!was_on && now_on) {
		say_info("wal_ext was enabled dynamically; "
			 "triggering checkpoint for clean CDC bootstrap");
		if (box_checkpoint() != 0)
			return luaT_error(L);
	}
	return 0;
}

void
box_lua_wal_ext_init(struct lua_State *L)
{
	static const struct luaL_Reg wal_ext_internal[] = {
		{"cfg_set_wal_ext", cfg_set_wal_ext},
		{NULL, NULL}
	};

	luaL_findtable(L, LUA_GLOBALSINDEX, "box.internal", 0);
	luaL_setfuncs(L, wal_ext_internal, 0);
	lua_pop(L, 1);
}
