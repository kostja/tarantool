#include "lua.h"
#include "luajit/src/lauxlib.h"
#include "lua/utils.h"
#include <cfg.h>
#include "box/engine.h"
#include "box/journal.h"
#include "box/lua/misc.h"
#include "box/wal_ext.h"
#include "box/space_cache.h"

/**
 * Apply a dynamic change to box.cfg.wal_ext. Called only on
 * runtime reconfigure -- the initial box.cfg{} is intentionally
 * skipped via dynamic_cfg_skip_at_load, because the boot path
 * in main.cc already plumbs the initial value into
 * wal_ext_set_cfg() and no existing spaces are around to need
 * refresh.
 *
 * On a false -> true transition we quiesce in-flight writers so
 * that a subsequent snapshot is a clean CDC bootstrap point: every
 * WAL row after such a snapshot carries the freshly-enabled
 * extension. Taking that snapshot is left to the caller -- it is a
 * heavy operation that must not run inside this reconfigure path.
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
		/*
		 * Quiesce pre-flip writers so a later snapshot is a clean
		 * CDC boundary. A vinyl writer may have taken the
		 * blind-write fast path (skipping the old-tuple read)
		 * before the flip above; if it committed afterwards its
		 * WAL row would lack the old tuple. Abort in-flight writers
		 * that have not reached WAL yet -- there is no yield since
		 * the flip, so they all predate it. engine_switch_to_ro()
		 * reuses the existing abort-ready-writers path (memtx is a
		 * no-op, as it always carries the old tuple). Prepared
		 * writers cannot be aborted; journal_sync() flushes them so
		 * they are durable before we return. After this no pre-flip
		 * blind write is left pending.
		 */
		engine_switch_to_ro();
		if (journal_sync(NULL) != 0) {
			/*
			 * box.cfg's rollback only restores the Lua-visible
			 * option, so undo the engine-side mutation here too,
			 * otherwise the global flag and every space->wal_ext
			 * would stay enabled while box.cfg.wal_ext reads
			 * 'off'. The aborted writers above simply retry.
			 */
			struct wal_extensions_config revert = {
				.new_old = was_on,
			};
			wal_ext_set_cfg(&revert);
			space_cache_refresh_wal_ext();
			return luaT_error(L);
		}
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
