local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({
        env = {
            TARANTOOL_RUN_BEFORE_BOX_CFG = [[
local ffi = require('ffi')
ffi.cdef('void srand(unsigned int seed);')
ffi.C.srand(1)
math.randomseed(1)
            ]]
        }
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_LOG_FLUSH_DELAY', false)
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        box.error.injection.set('ERRINJ_VY_TASK_COMPLETE', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

--
-- Test that a dump happening while compaction is in progress correctly
-- adds a new slice to a range that has been temporarily removed from
-- the compaction heap (stray range).
--
g.test_dump_during_compaction = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {run_count_per_level = 1})

        -- Block compaction and create 2 runs to trigger it.
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)
        s:replace({1, 1})
        box.snapshot()
        s:replace({1, 2})
        box.snapshot()

        -- Wait for compaction to start (blocked in execute).
        t.helpers.retrying({}, function()
            t.assert_gt(box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- Trigger a dump while compaction is in progress.
        -- The range is stray (removed from the compaction heap).
        s:replace({1, 100})
        box.snapshot()

        -- Resume compaction and wait for everything to complete.
        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        t.helpers.retrying({}, function()
            local stat = box.stat.vinyl().scheduler
            t.assert_gt(stat.tasks_completed, tc)
            t.assert_equals(stat.tasks_inprogress, 0)
        end)

        -- The dump slice must survive compaction.
        t.assert_equals(s:get{1}, {1, 100})
        t.assert_equals(box.stat.vinyl().scheduler.tasks_failed, 0)
    end)
end

--
-- Test that after a compaction task fails, the compaction plan is
-- rebuilt from scratch and the range is re-scheduled for compaction.
--
g.test_compaction_abort_retries = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {run_count_per_level = 1})

        -- Block compaction and create 3 runs.
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)
        for i = 1, 3 do
            s:replace({1, i})
            box.snapshot()
        end

        -- Make the next task completion fail.
        box.error.injection.set('ERRINJ_VY_TASK_COMPLETE', true)
        box.stat.reset()

        -- Unblock compaction: the task executes in the worker but
        -- the completion hook fails, triggering abort.
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        t.helpers.retrying({}, function()
            t.assert_ge(box.stat.vinyl().scheduler.tasks_failed, 1)
        end)

        -- Disable error injection and let the retry succeed.
        box.error.injection.set('ERRINJ_VY_TASK_COMPLETE', false)
        t.helpers.retrying({}, function()
            t.assert_ge(box.stat.vinyl().scheduler.tasks_completed, 1)
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        t.assert_equals(s:get{1}, {1, 3})
    end)
end

--
-- Test that force compaction on a single-slice range is a no-op:
-- needs_compaction is cleared because there is nothing to compact.
--
g.test_force_compact_single_slice = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- Create a single run.
        s:replace({1, 1})
        box.snapshot()

        -- Force compaction. With only one slice, the scheduler
        -- should clear needs_compaction and not schedule a task.
        box.stat.reset()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_completed, 0)
        end)
    end)
end

--
-- Test that partial compaction (not all slices included) preserves
-- tombstones. If is_last_level were incorrectly set to true, the
-- tombstone would be dropped and the deleted key would resurface
-- after a subsequent major compaction.
--
g.test_partial_compaction_preserves_tombstones = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 1,
            run_size_ratio = 10,
        })

        -- Block compaction.
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)

        -- Create a large last-level run containing key 1.
        for k = 1, 1000 do
            s:replace({k, string.rep('x', 1000)})
        end
        box.snapshot()

        -- Create 3 small runs on top: update key 1, then delete it.
        s:replace({1, 'y'})
        box.snapshot()
        s:replace({1, 'z'})
        box.snapshot()
        s:delete({1})
        box.snapshot()

        -- Unblock: only the 3 small runs overflow L1 and get
        -- compacted. The large run stays at a deeper level
        -- (is_last_level = false for this compaction).
        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        t.helpers.retrying({}, function()
            local stat = box.stat.vinyl().scheduler
            t.assert_gt(stat.tasks_completed, tc)
            t.assert_equals(stat.tasks_inprogress, 0)
        end)

        -- After partial compaction: compacted small runs + big run.
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Now force a full (major) compaction of all slices.
        s.index.pk:compact()
        t.helpers.retrying({}, function()
            t.assert_equals(s.index.pk:stat().run_count, 1)
        end)

        -- Key 1 must not exist: the partial compaction preserved
        -- the DELETE tombstone (is_last_level = false), so the
        -- full compaction correctly suppressed the old INSERT from
        -- the large run. If the partial compaction had wrongly
        -- dropped the tombstone, key 1 would resurface here.
        t.assert_equals(s:get{1}, nil)
    end)
end
