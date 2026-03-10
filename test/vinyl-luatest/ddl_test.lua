local t = require('luatest')
local server = require('luatest.server')
local g = t.group('ddl_debug')

g.before_each(function(cg)
    cg.server = server:new({
        box_cfg = {
            log_level = 'verbose',
        },
    })
    cg.server:start()
end)

g.after_each(function(cg)
    cg.server:drop()
end)

-- Regression test: verify that secondary index counts stay
-- consistent with the primary index during concurrent DDL + DML.
--
-- The bug was in vy_compaction_plan_check_last_level() which only
-- checked slices older than the oldest plan slice for overlap.
-- After trim, non-plan slices can appear BETWEEN plan slices in
-- the linked list, so they were silently skipped.  This caused
-- incorrect is_last_level=true, dropping DELETE statements during
-- PK compaction and resurrecting old entries (pk > sk).
g.test_sk_count_mismatch = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')

        math.randomseed(os.time())

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- Pre-populate with 1000 entries, snapshotting every 300
        -- to create multiple run files.
        box.begin()
        for i = 1, 1000 do
            if i % 100 == 0 then
                box.commit()
                box.begin()
            end
            if i % 300 == 0 then
                box.snapshot()
            end
            s:replace{i, i, i}
        end
        box.commit()

        local last_val = 1000
        local ch = fiber.channel(1)

        local function gen_load()
            for _ = 1, 200 do
                local op = math.random(4)
                local key = math.random(1000)
                local val1 = math.random(1000)
                local val2 = last_val + 1
                last_val = val2
                if op == 1 then
                    pcall(s.insert, s, {key, val1, val2})
                elseif op == 2 then
                    pcall(s.replace, s, {key, val1, val2})
                elseif op == 3 then
                    pcall(s.delete, s, {key})
                elseif op == 4 then
                    pcall(s.upsert, s, {key, val1, val2},
                          {{'=', 2, val1}, {'=', 3, val2}})
                end
            end
        end

        -- Phase 1: build SK with concurrent DML.
        fiber.create(function() gen_load() ch:put(true) end)
        s:create_index('sk', {unique = false, parts = {2, 'unsigned'}})
        ch:get(10)

        t.assert_equals(s.index.pk:count(), s.index.sk:count(),
                        "Phase 1: pk == sk")

        -- Phase 2: build TK with concurrent DML.
        fiber.create(function() gen_load() ch:put(true) end)
        s:create_index('tk', {unique = true, parts = {3, 'unsigned'}})
        ch:get(10)

        local pk_count = s.index.pk:count()
        local sk_count = s.index.sk:count()
        local tk_count = s.index.tk:count()
        t.assert_equals(pk_count, sk_count,
            string.format("Phase 2: pk(%d) == sk(%d)", pk_count, sk_count))
        t.assert_equals(pk_count, tk_count,
            string.format("Phase 2: pk(%d) == tk(%d)", pk_count, tk_count))
    end)
end
