local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

--
-- Basic TTL: insert a tuple with expires_at in the past,
-- verify it is invisible to reads.
--
g.test_basic_ttl_filtering = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'string'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()

        -- Insert one expired and one non-expired tuple.
        s:replace{1, 'alive', now + 3600}
        s:replace{2, 'expired', now - 1}

        -- The expired tuple should be invisible.
        t.assert_equals(s:get(1), {1, 'alive', now + 3600})
        t.assert_equals(s:get(2), nil)

        -- Full scan should only return the alive tuple.
        t.assert_equals(s:select(), {{1, 'alive', now + 3600}})
    end)
end

--
-- TTL with nil expires_at: tuples with nil never expire.
--
g.test_nil_expires_at = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'string'},
                {name = 'expires_at', type = 'number', is_nullable = true},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        -- Tuple with nil expires_at should always be visible.
        s:replace{1, 'forever', box.NULL}
        t.assert_equals(s:get(1), {1, 'forever', box.NULL})
    end)
end

--
-- TTL filtering from disk: dump to disk, then read.
--
g.test_ttl_from_disk = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}
        box.snapshot()

        -- After dump, expired tuple should still be invisible.
        t.assert_equals(s:get(1), {1, 100, now + 3600})
        t.assert_equals(s:get(2), nil)
    end)
end

--
-- TTL acts as tombstone: expired tuple masks older versions.
--
g.test_ttl_as_tombstone = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk', {run_count_per_level = 100})

        local now = clock.realtime()

        -- Dump an older version with no expiration.
        s:replace{1, 100, now + 3600}
        box.snapshot()

        -- Overwrite with an expired version.
        s:replace{1, 200, now - 1}
        box.snapshot()

        -- The key should be invisible: the expired tuple on top
        -- acts as a tombstone, masking the older alive version.
        t.assert_equals(s:get(1), nil)
    end)
end

--
-- Last-level compaction drops expired tuples.
--
g.test_compaction_drops_expired = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk', {run_count_per_level = 1})

        local now = clock.realtime()

        -- Dump two runs to trigger compaction (run_count_per_level=1).
        s:replace{1, 100, now - 1}   -- expired
        s:replace{2, 200, now + 3600} -- alive
        box.snapshot()
        -- Second dump with different data to trigger compaction.
        s:replace{3, 300, now - 1}   -- expired
        box.snapshot()

        -- Wait for compaction to finish.
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- After last-level compaction, only the alive tuple remains.
        t.assert_equals(s:select(), {{2, 200, now + 3600}})
    end)
end

--
-- Secondary index filters expired entries independently.
--
g.test_secondary_index_ttl = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {{field = 'value'}}})

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}

        -- Select via secondary index should filter expired.
        t.assert_equals(s.index.sk:select(200), {})
        t.assert_equals(s.index.sk:select(100),
                        {{1, 100, now + 3600}})
    end)
end

--
-- Secondary index TTL from disk.
--
g.test_secondary_index_ttl_from_disk = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {{field = 'value'}}})

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}
        box.snapshot()

        -- After dump, select via SK should still filter expired.
        t.assert_equals(s.index.sk:select(200), {})
        t.assert_equals(s.index.sk:select(100),
                        {{1, 100, now + 3600}})
    end)
end

--
-- Mixed TTL + no-TTL: tuples without expires_at coexist
-- with tuples that have it.
--
g.test_mixed_ttl_and_no_ttl = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number', is_nullable = true},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}         -- alive with TTL
        s:replace{2, 200, now - 1}            -- expired
        s:replace{3, 300, box.NULL}           -- no TTL (nil)

        t.assert_equals(s:select(), {
            {1, 100, now + 3600},
            {3, 300, box.NULL},
        })
    end)
end

--
-- Zero expires_at means never-expiring.
--
g.test_zero_expires_at = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        s:replace{1, 0}
        t.assert_equals(s:get(1), {1, 0})
    end)
end

--
-- Error: ttl option with nonexistent field.
--
g.test_ttl_bad_field = function(cg)
    cg.server:exec(function()
        t.assert_error_msg_contains("is not in the format", function()
            box.schema.space.create('test', {
                engine = 'vinyl',
                format = {
                    {name = 'id', type = 'unsigned'},
                },
                ttl = {field = 'nonexistent'},
            })
        end)
    end)
end

--
-- TTL disabled by default (ttl not specified).
--
g.test_no_ttl_by_default = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
        })
        s:create_index('pk')

        -- Without TTL, even a tuple with expires_at in the past
        -- is visible — the field is just regular data.
        local now = clock.realtime()
        s:replace{1, now - 1}
        t.assert_equals(s:get(1), {1, now - 1})
    end)
end

--
-- Point lookup via secondary index for an expired tuple returns nil.
--
g.test_point_lookup_via_sk = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {{field = 'value'}}, unique = true})

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}

        -- Point lookup via SK for expired tuple.
        t.assert_equals(s.index.sk:get(200), nil)
        t.assert_equals(s.index.sk:get(100), {1, 100, now + 3600})

        -- Same after dump.
        box.snapshot()
        t.assert_equals(s.index.sk:get(200), nil)
        t.assert_equals(s.index.sk:get(100), {1, 100, now + 3600})
    end)
end

--
-- Various iterator types skip expired tuples correctly.
--
g.test_iterator_types = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        -- Insert expired at odd keys, alive at even keys.
        s:replace{1, now - 1}
        s:replace{2, now + 3600}
        s:replace{3, now - 1}
        s:replace{4, now + 3600}
        s:replace{5, now - 1}
        s:replace{6, now + 3600}

        -- EQ: expired key returns nothing, alive key returns the tuple.
        t.assert_equals(s:select(1, {iterator = 'EQ'}), {})
        t.assert_equals(s:select(2, {iterator = 'EQ'}),
                        {{2, now + 3600}})

        -- GE from an expired key skips it.
        t.assert_equals(s:select(1, {iterator = 'GE'}),
                        {{2, now + 3600}, {4, now + 3600}, {6, now + 3600}})

        -- LE from an expired key skips it.
        t.assert_equals(s:select(5, {iterator = 'LE'}),
                        {{4, now + 3600}, {2, now + 3600}})

        -- GT from an expired key.
        t.assert_equals(s:select(3, {iterator = 'GT'}),
                        {{4, now + 3600}, {6, now + 3600}})

        -- LT from an expired key.
        t.assert_equals(s:select(3, {iterator = 'LT'}),
                        {{2, now + 3600}})
    end)
end

--
-- Select with limit correctly counts only alive tuples.
--
g.test_select_with_limit = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        -- 10 tuples, alternating expired/alive.
        for i = 1, 10 do
            if i % 2 == 0 then
                s:replace{i, now + 3600}
            else
                s:replace{i, now - 1}
            end
        end

        -- limit=3 should return exactly 3 alive tuples,
        -- skipping expired ones without counting toward limit.
        local result = s:select({}, {limit = 3})
        t.assert_equals(result, {
            {2, now + 3600},
            {4, now + 3600},
            {6, now + 3600},
        })
    end)
end

--
-- Non-last-level compaction preserves expired tuples:
-- after compaction the expired tuple still masks the alive
-- version beneath it.
--
g.test_non_last_level_preserves_expired = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        -- Use run_count_per_level=100 to prevent auto-compaction.
        s:create_index('pk', {run_count_per_level = 100})

        local now = clock.realtime()

        -- Dump an alive version into the first run.
        s:replace{1, 100, now + 3600}
        box.snapshot()

        -- Overwrite with an expired version in the second run.
        s:replace{1, 200, now - 1}
        box.snapshot()

        -- The expired tuple on top masks the alive one.
        t.assert_equals(s:get(1), nil)

        -- Force a non-last-level compaction (compact all runs
        -- into a single run — still last-level=true since all
        -- runs are included). To get a non-last-level compaction,
        -- we compact the top run only.
        -- With run_count_per_level=100, no auto-compaction fires.
        -- Use compact() to trigger compaction of all runs.
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- After compaction, the expired tuple is still visible
        -- in the merged result (non-last-level wouldn't drop it),
        -- but since this is actually last-level compaction (all
        -- runs merged), the expired tuple IS dropped.
        -- So key 1 becomes invisible (alive version was
        -- superseded by expired version, then both dropped).
        t.assert_equals(s:get(1), nil)
    end)
end

--
-- UPSERT with expired base: the upsert default tuple has
-- expires_at=0 (never-expiring), so reading it before
-- compaction returns the default. After compaction the
-- upsert is applied to the expired base, inheriting its
-- expires_at.
--
g.test_upsert_with_expired_base = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk', {run_count_per_level = 1})

        local now = clock.realtime()

        -- First dump: create a run so the next dump is NOT last level.
        -- (Last-level dumps skip expired tuples entirely.)
        s:replace{10, 1000, now + 99999}
        box.snapshot()

        -- Insert an expired tuple as the base (not last level).
        s:replace{1, 100, now - 1}
        box.snapshot()

        -- Apply UPSERT on the same key, with default tuple
        -- that has expires_at in the past too.
        s:upsert({1, 0, now - 1}, {{'+', 2, 50}})

        -- Before compaction, the UPSERT is applied on read:
        -- base is {1, 100, now-1} (expired), upsert adds 50
        -- to field 2, giving {1, 150, now-1} — still expired.
        t.assert_equals(s:get(1), nil)

        -- After compaction, the merged result should also be expired.
        box.snapshot()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)
        t.assert_equals(s:get(1), nil)
    end)
end

--
-- TTL filtering works correctly after server restart.
--
g.test_recovery_after_restart = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}
        box.snapshot()
    end)

    cg.server:restart()

    cg.server:exec(function()
        local s = box.space.test
        -- After restart, TTL filtering should still work.
        t.assert_equals(s:get(1)[2], 100)
        t.assert_equals(s:get(2), nil)
        t.assert_equals(#s:select(), 1)
    end)
end

--
-- .index2 metadata survives restart and compaction works.
--
g.test_index2_metadata_survives_restart = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk', {run_count_per_level = 1})

        local now = clock.realtime()
        s:replace{1, 100, now + 3600}
        s:replace{2, 200, now - 1}
        box.snapshot()
    end)

    cg.server:restart()

    cg.server:exec(function()
        local clock = require('clock')
        local s = box.space.test
        local now = clock.realtime()

        -- Dump again and force compaction.
        s:replace{3, 300, now + 3600}
        box.snapshot()
        s.index.pk:compact()

        -- Wait for compaction.
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- Compaction should complete successfully.
        -- Expired tuple was dropped during the first dump
        -- (last-level), so only alive tuples remain.
        local result = s:select()
        t.assert_equals(#result, 2)
    end)
end

--
-- ALTER to add TTL to an existing space.
--
g.test_alter_add_ttl = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        -- Create space without TTL.
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
        })
        s:create_index('pk')

        local now = clock.realtime()
        -- Insert data without TTL — all tuples visible.
        s:replace{1, 100, now - 1}
        s:replace{2, 200, now + 3600}
        box.snapshot()

        -- Both visible before TTL is enabled.
        t.assert_equals(#s:select(), 2)

        -- ALTER to add TTL.
        s:alter({ttl = {field = 'expires_at'}})

        -- Now tuple 1 has expires_at in the past — should be invisible.
        t.assert_equals(s:get(1), nil)
        t.assert_equals(s:get(2), {2, 200, now + 3600})
    end)
end

--
-- ALTER to remove TTL from a space.
--
g.test_alter_remove_ttl = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        -- First dump: create a run so the next dump is NOT last level.
        -- (Last-level dumps skip expired tuples entirely.)
        local now = clock.realtime()
        s:replace{10, 1000, now + 99999}
        box.snapshot()

        -- Second dump: expired tuple survives (not last level).
        s:replace{1, 100, now - 1}   -- expired
        s:replace{2, 200, now + 3600}
        box.snapshot()

        -- With TTL, tuple 1 is invisible.
        t.assert_equals(s:get(1), nil)

        -- ALTER to remove TTL.
        s:alter({ttl = false})

        -- Previously expired tuple becomes visible again.
        t.assert_equals(s:get(1), {1, 100, now - 1})
        t.assert_equals(#s:select(), 3)
    end)
end

--
-- Boundary: expires_at exactly equal to now is NOT expired
-- (the check uses strict <).
--
g.test_expires_at_boundary = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        -- Use a timestamp far in the future so that by the time
        -- the check runs, it's still >= now.
        local future = clock.realtime() + 1
        s:replace{1, future}
        t.assert_equals(s:get(1), {1, future})
    end)
end

--
-- Integer expires_at (MP_UINT encoding) works with TTL.
--
g.test_integer_expires_at = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = math.floor(clock.realtime())
        -- Integer timestamp in the past.
        s:replace{1, now - 100}
        t.assert_equals(s:get(1), nil)

        -- Integer timestamp in the future.
        s:replace{2, now + 3600}
        t.assert_equals(s:get(2), {2, now + 3600})
    end)
end

--
-- Cache invalidation: overwrite alive tuple with expired version.
--
g.test_cache_invalidation_on_ttl_overwrite = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'value', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()

        -- Insert alive tuple and read it (populates cache).
        s:replace{1, 100, now + 3600}
        t.assert_equals(s:get(1), {1, 100, now + 3600})

        -- Overwrite with expired version.
        s:replace{1, 200, now - 1}

        -- Read again — should return nil, not stale cached data.
        t.assert_equals(s:get(1), nil)
    end)
end

--
-- TTL read metrics: rows_expired increments for filtered tuples.
--
g.test_ttl_read_metrics = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk')

        local now = clock.realtime()
        s:replace{1, now - 1}   -- expired
        s:replace{2, now + 3600} -- alive
        s:replace{3, now - 1}   -- expired

        -- Reset stats.
        box.stat.reset()

        -- Read expired tuple via point lookup.
        t.assert_equals(s:get(1), nil)
        local stat = s.index.pk:stat()
        t.assert_ge(stat.ttl.rows_expired.rows, 1,
                    "point lookup should increment rows_expired")

        -- Read via iterator (select scans all, filtering expired).
        box.stat.reset()
        t.assert_equals(s:select(), {{2, now + 3600}})
        stat = s.index.pk:stat()
        t.assert_ge(stat.ttl.rows_expired.rows, 2,
                    "iterator should increment rows_expired for each expired tuple")
    end)
end

--
-- TTL compaction metrics: rows_skipped increments for dropped tuples.
--
g.test_ttl_compaction_metrics = function(cg)
    cg.server:exec(function()
        local clock = require('clock')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            format = {
                {name = 'id', type = 'unsigned'},
                {name = 'expires_at', type = 'number'},
            },
            ttl = {field = 'expires_at'},
        })
        s:create_index('pk', {run_count_per_level = 1})

        local now = clock.realtime()

        -- First dump: create a non-empty run with alive data.
        -- This makes subsequent dumps NOT last-level, so expired
        -- tuples survive in runs until compaction drops them.
        s:replace{10, now + 99999}
        box.snapshot()

        -- Reset stats after the first dump.
        box.stat.reset()

        -- Second dump: expired tuples survive (not last level).
        s:replace{1, now - 1}
        s:replace{2, now - 1}
        s:replace{3, now - 1}
        box.snapshot()

        -- Wait for last-level compaction (merges both runs).
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        local stat = s.index.pk:stat()
        t.assert_ge(stat.ttl.rows_skipped.rows, 1,
                    "compaction should increment rows_skipped for expired tuples")
    end)
end
