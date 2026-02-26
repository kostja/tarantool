local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({
        alias = 'master',
        env = {
            TARANTOOL_RUN_BEFORE_BOX_CFG = [[
local ffi = require('ffi')
ffi.cdef('void cord_set_seed(unsigned int seed);')
ffi.C.cord_set_seed(43948017)
math.randomseed(1)
            ]]
        },
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

--
-- Checks that stale tuples are deleted from the secondary index in case there
-- are UPSERT statements in the space (gh-3638).
--
g.test_defer_deletes_and_upserts = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local wait_compaction = function(tasks_completed)
            while true do
                local stat = box.stat.vinyl().scheduler
                if stat.tasks_completed > tasks_completed and
                    stat.tasks_inprogress == 0 and
                    stat.compaction_queue == 0 then
                    break
                end
                fiber.sleep(0.01)
            end
        end
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            defer_deletes = true,
        })
        s:create_index('pk', {
            run_count_per_level = 100, -- disables auto-compaction
        })
        -- Add some padding to make sure that the runs will be assigned to
        -- different levels.
        --
        -- Write the first run with REPLACE in it.
        s:replace{1, 10, string.rep('x', 1000)}
        box.snapshot()
        -- Write the second run with UPSERT in it.
        s:upsert({1, 10, string.rep('x', 100)}, {{'+', 2, 10}})
        box.snapshot()
        -- Create a non-unique secondary index.
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        -- Write the third run with DELETE in it.
        s:delete{1}
        local tasks_completed = box.stat.vinyl().scheduler.tasks_completed
        box.snapshot()
        wait_compaction(tasks_completed)
        -- Check that no compaction is in progress and the primary index
        -- have three runs with REPLACE, UPSERT, and DELETE.
        t.assert_covers(box.stat.vinyl().scheduler, {
            tasks_inprogress = 0, compaction_queue = 0,
        })
        t.assert_equals(s.index.pk:stat().run_count, 3)
        t.assert_equals(s.index.pk:stat().disk.statement, {
            inserts = 0, replaces = 1, deletes = 1, upserts = 1,
        })
        -- Compact the primary index, then the secondary index.
        -- Check that all runs are deleted.
        s.index.pk:compact()
        t.helpers.retrying({}, function()
            t.assert_equals(s.index.pk:stat().run_count, 0)
        end)
        s.index.sk:compact()
        t.helpers.retrying({}, function()
            t.assert_equals(s.index.sk:stat().run_count, 0)
        end)
        s:drop()
    end)
end
