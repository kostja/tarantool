local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({
        env = {
            TARANTOOL_RUN_BEFORE_BOX_CFG = [[
local ffi = require('ffi')
ffi.cdef('void cord_set_seed(unsigned int seed);')
ffi.C.cord_set_seed(43948017)
math.randomseed(1)
            ]]
        }
    })
    cg.server:start()
    cg.server:exec(function()
        rawset(_G, 'wait_compaction', function(tasks_completed)
            local fiber = require('fiber')
            local deadline = fiber.time() + 60
            while fiber.time() < deadline do
                local stat = box.stat.vinyl().scheduler
                if stat.tasks_completed > tasks_completed and
                    stat.idle == 1 then
                    return
                end
                fiber.sleep(0.01)
            end
            error('wait_compaction: timed out after 60 seconds')
        end)
    end)
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        box.error.injection.set('ERRINJ_VY_LOG_FLUSH_DELAY', false)
        box.error.injection.set('ERRINJ_VY_TASK_COMPLETE', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

-- A read view opened before compaction must see pre-compaction
-- data even after trim changes which slices get compacted.
g.test_read_view_survives_compaction = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 10,
            range_size = 64 * 1024,
        })

        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)

        -- Pad tuples so the range exceeds the trim threshold.
        local pad = string.rep('x', 150)
        for k = 1, 100 do s:replace({k, 'v1', pad}) end
        box.snapshot()

        -- Overlaps dump 1.
        for k = 50, 150 do s:replace({k, 'v2', pad}) end
        box.snapshot()

        -- Disjoint from dumps 1 & 2.
        for k = 200, 300 do s:replace({k, 'v3', pad}) end
        box.snapshot()

        -- Open a read view via select() before compaction,
        -- then verify it still sees the same data after.
        local ch = fiber.channel(1)
        local rv_results = {}
        local reader = fiber.new(function()
            box.begin()
            -- Pin the read view with a full scan.
            for _, tuple in s:pairs() do
                table.insert(rv_results, {tuple[1], tuple[2]})
            end
            ch:put('pinned')
            -- Wait for compaction to finish.
            ch:get()
            -- Re-scan through the same read view.
            local rescan = {}
            for _, tuple in s:pairs() do
                table.insert(rescan, {tuple[1], tuple[2]})
            end
            box.commit()
            -- Both scans must return the same data.
            t.assert_equals(rescan, rv_results)
        end)
        reader:set_joinable(true)
        t.assert_equals(ch:get(), 'pinned')

        -- Insert new data and compact while the read view is open.
        for k = 1, 100 do s:replace({k, 'v4', pad}) end
        box.snapshot()

        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        s.index.pk:compact()
        _G.wait_compaction(tc)

        -- Resume the reader and wait for it to finish.
        ch:put('done')
        reader:join()

        -- Read view must see pre-compaction data.
        t.assert_equals(#rv_results, 251)
        local by_key = {}
        for _, r in ipairs(rv_results) do by_key[r[1]] = r[2] end
        t.assert_equals(by_key[1], 'v1')
        t.assert_equals(by_key[50], 'v2')
        t.assert_equals(by_key[200], 'v3')

        -- Fresh read must see post-compaction data.
        t.assert_equals(s:get(1):totable(), {1, 'v4', pad})
    end)
end

-- Overlapping and disjoint dumps are compacted correctly when
-- new data is added between dumps and compaction.
g.test_compaction_with_overlap = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 10,
            range_size = 64 * 1024,
        })

        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)

        local pad = string.rep('x', 150)
        for k = 1, 100 do s:replace({k, 'v1', pad}) end
        box.snapshot()

        -- Overlaps dump 1.
        for k = 50, 150 do s:replace({k, 'v2', pad}) end
        box.snapshot()

        -- Disjoint from dumps 1 & 2.
        for k = 200, 300 do s:replace({k, 'v3', pad}) end
        box.snapshot()

        -- Overwrites keys from dump 1.
        for k = 1, 100 do s:replace({k, 'v4', pad}) end
        box.snapshot()

        t.assert_equals(s.index.pk:stat().run_count, 4)

        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        s.index.pk:compact()
        _G.wait_compaction(tc)

        t.assert_equals(s:get(1):totable(), {1, 'v4', pad})
        t.assert_equals(s:get(50):totable(), {50, 'v4', pad})
        t.assert_equals(s:get(101):totable(), {101, 'v2', pad})
        t.assert_equals(s:get(200):totable(), {200, 'v3', pad})
    end)
end

-- Mixed overlapping and disjoint dumps: only the overlapping
-- cluster is compacted, the disjoint run survives.
g.test_mixed_overlap_compaction = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 10,
            range_size = 64 * 1024,
        })

        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)

        local pad = string.rep('x', 150)
        for k = 1, 100 do s:replace({k, 'a', pad}) end
        box.snapshot()

        -- Overlaps dump 1.
        for k = 50, 150 do s:replace({k, 'b', pad}) end
        box.snapshot()

        -- Disjoint from dumps 1 & 2.
        for k = 500, 600 do s:replace({k, 'c', pad}) end
        box.snapshot()

        t.assert_equals(s.index.pk:stat().run_count, 3)

        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        s.index.pk:compact()
        _G.wait_compaction(tc)

        -- Two overlapping runs compacted into one, disjoint stays.
        t.assert_le(s.index.pk:stat().run_count, 2)

        t.assert_equals(s:get(1):totable(), {1, 'a', pad})
        t.assert_equals(s:get(50):totable(), {50, 'b', pad})
        t.assert_equals(s:get(101):totable(), {101, 'b', pad})
        t.assert_equals(s:get(500):totable(), {500, 'c', pad})
    end)
end

--
-- The scheduler must not hang when a range split happens
-- concurrently with a dump.
--
-- The scheduler may yield during a range split (vy_log I/O).
-- While it yields, a dump worker may complete its task and
-- signal scheduler_cond.  Since the scheduler is not in
-- fiber_cond_wait at that moment, the signal is lost and the
-- scheduler hangs in fiber_cond_wait on the next iteration.
--
-- The test uses ERRINJ_VY_LOG_FLUSH_DELAY to widen the yield
-- window during the split, ensuring the dump completion arrives
-- while the scheduler is blocked on the vy_log latch, making
-- the race deterministic.
--
g.test_no_scheduler_hang_after_split = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local errinj = box.error.injection

        local value = string.rep('x', 2000)
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 128,
            -- Large enough that the compacted run (~20KB) does not
            -- trigger a split, but small enough that adding the
            -- concurrent dump (~20KB more) does.
            range_size = 16384,
        })

        -- Step 1: Create one large compacted run R (~20KB).
        for i = 1, 10 do s:replace{i, value} end
        box.snapshot()
        for i = 1, 10 do s:replace{i, string.rep('y', 2000)} end
        box.snapshot()

        s.index.pk:compact()
        while s.index.pk:stat().disk.compaction.count < 1 do
            fiber.sleep(0.01)
        end
        t.assert_equals(s.index.pk:stat().range_count, 1)
        t.assert_equals(s.index.pk:stat().run_count, 1)

        -- Step 2: Add a small disjoint run.  Key 500 is far from
        -- keys 1-10, so the two slices are disjoint.  The 2KB
        -- run is much smaller than R (~20KB), so the shape-based
        -- compaction doesn't trigger (they land at different
        -- levels in the LSM tree).
        s:replace{500, value}
        box.snapshot()
        t.assert_equals(s.index.pk:stat().range_count, 1)
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Step 3: Insert data for the concurrent dump.
        for i = 11, 20 do s:replace{i, value} end

        -- Start box.snapshot() in a fiber and yield once so
        -- it bumps the dump generation before the scheduler
        -- runs.  This ensures peek_dump submits the dump task
        -- before peek_compaction enters the split path.
        local f = fiber.new(function() box.snapshot() end)
        f:set_joinable(true)
        fiber.sleep(0)

        -- Now the generation is bumped.  Set up the race:
        -- the injection blocks vy_log_tx_flush, and compact()
        -- gives the range compaction priority (needs_compaction
        -- with 2 disjoint slices → trim to plan.count=1,
        -- enough to enter peek_compaction).
        --
        -- The scheduler runs peek_dump first (submits the dump
        -- task to the worker), then peek_compaction.  The range
        -- is ~22KB, well above range_size=4096, so it calls
        -- vy_lsm_split_range.  The split yields in
        -- vy_log_tx_commit, blocked by the injection.
        -- Meanwhile, the dump worker completes and signals
        -- scheduler_cond.  Since the scheduler is not in
        -- fiber_cond_wait, the signal is lost.
        --
        -- After the split, each new range gets at most one of
        -- the two disjoint slices, so no compaction task is
        -- created.  Without the fix, the scheduler enters
        -- fiber_cond_wait with a completed dump task sitting
        -- in processed_tasks, hanging forever.
        errinj.set('ERRINJ_VY_LOG_FLUSH_DELAY', true)
        s.index.pk:compact()

        -- The wait for the dump worker is probabilistic: the
        -- scheduler is blocked in vy_log_tx_flush and there is
        -- no Lua-visible metric for completed-but-unprocessed
        -- tasks in the scheduler queue.  10ms is sufficient for
        -- the worker to write ~20KB to disk in practice.
        fiber.sleep(0.01)

        errinj.set('ERRINJ_VY_LOG_FLUSH_DELAY', false)

        -- With the fix, box.snapshot() completes promptly.
        -- Without it, the scheduler is stuck in fiber_cond_wait.
        t.assert_equals({f:join(10)}, {true})
    end)
end

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
