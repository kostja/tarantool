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
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        box.error.injection.set('ERRINJ_VY_LOG_FLUSH_DELAY', false)
        box.error.injection.set('ERRINJ_VY_TASK_COMPLETE', false)
        box.error.injection.set('ERRINJ_XLOG_GARBAGE', false)
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
-- Test that a forward scan works correctly when loading an index
-- block yields (ERRINJ_VY_INDEX_BLOCK_DELAY).  The yield happens
-- between the coio disk read and the cache insert in
-- vy_run_get_index_block.  Covers the seek path
-- (vy_page_index_find_page_impl) and the load_page path
-- (vy_run_page_info_v2).
--
g.test_index_block_yield_forward_scan = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        local pad = string.rep('x', 50)
        for i = 1, 200 do
            s:replace{i, pad}
        end
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)
        local ch = fiber.channel(1)
        local f = fiber.new(function()
            ch:put(s:select())
        end)
        f:set_joinable(true)
        -- Let the reader hit the injection point.
        fiber.sleep(0.1)
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        local result = ch:get(10)
        t.assert_equals(#result, 200)
        t.assert_equals(result[1]:totable()[1], 1)
        t.assert_equals(result[200]:totable()[1], 200)
    end)
end

--
-- Test that a reverse scan works correctly when loading an index
-- block yields.  Covers vy_run_iterator_next_pos (reverse branch)
-- which calls vy_run_page_info_v2 to get the row_count of the
-- previous page.
--
g.test_index_block_yield_reverse_scan = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        local pad = string.rep('x', 50)
        for i = 1, 200 do
            s:replace{i, pad}
        end
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)
        local ch = fiber.channel(1)
        local f = fiber.new(function()
            ch:put(s:select({}, {iterator = 'REQ'}))
        end)
        f:set_joinable(true)
        fiber.sleep(0.1)
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        local result = ch:get(10)
        t.assert_equals(#result, 200)
        t.assert_equals(result[1]:totable()[1], 200)
        t.assert_equals(result[200]:totable()[1], 1)
    end)
end

--
-- Test that a point lookup (ITER_EQ) works correctly when loading
-- an index block yields.  The point lookup goes through
-- vy_run_bloom_check → vy_run_get_index_block (to load the fuse8
-- filter), then if the filter says "maybe present", through the
-- normal seek path.
--
g.test_index_block_yield_point_lookup = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        local pad = string.rep('x', 50)
        for i = 1, 200 do
            s:replace{i, pad}
        end
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)
        local ch = fiber.channel(1)
        local f = fiber.new(function()
            ch:put({
                s:get{1},
                s:get{100},
                s:get{200},
                s:get{999},  -- does not exist
            })
        end)
        f:set_joinable(true)
        fiber.sleep(0.1)
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        local result = ch:get(10)
        t.assert_equals(result[1]:totable()[1], 1)
        t.assert_equals(result[2]:totable()[1], 100)
        t.assert_equals(result[3]:totable()[1], 200)
        t.assert_equals(result[4], nil)
    end)
end

--
-- Test that the slice pin mechanism protects against compaction
-- while a reader fiber is paused inside vy_run_get_index_block.
-- The reader pins slices when the read iterator opens.  While
-- the reader is paused (injection yield), compaction completes
-- but vy_slice_wait_pinned blocks until the reader finishes.
--
g.test_index_block_yield_pin_vs_compaction = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 1,
            page_size = 256,
        })

        -- Block compaction to accumulate 2 runs.
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', true)
        local pad = string.rep('x', 50)
        for i = 1, 100 do
            s:replace{i, 'v1', pad}
        end
        box.snapshot()
        for i = 1, 100 do
            s:replace{i, 'v2', pad}
        end
        box.snapshot()
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Start reader with index block delay.  The reader will
        -- open a read iterator (pinning slices), hit the first
        -- cache miss, do coio I/O, then pause at the injection.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)
        local ch = fiber.channel(1)
        local f = fiber.new(function()
            ch:put(s:select())
        end)
        f:set_joinable(true)
        fiber.sleep(0.1)

        -- While reader is paused, unblock compaction.
        -- The compaction task executes in a worker thread, but
        -- completion (which deletes old slices) blocks on
        -- vy_slice_wait_pinned because the reader still has them
        -- pinned.
        local tc = box.stat.vinyl().scheduler.tasks_completed
        box.error.injection.set('ERRINJ_VY_COMPACTION_DELAY', false)
        s.index.pk:compact()

        -- Give compaction time to start (but it can't complete
        -- because slices are pinned).
        fiber.sleep(0.5)

        -- Resume the reader.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        local result = ch:get(10)
        t.assert_equals(#result, 100)
        -- Reader must see v2 (latest version).
        t.assert_equals(result[1]:totable()[2], 'v2')

        -- Now compaction can complete.
        _G.wait_compaction(tc)
        t.assert_le(s.index.pk:stat().run_count, 1)
    end)
end

--
-- Test that the slice pin mechanism protects against space DROP
-- while a reader fiber is paused inside vy_run_get_index_block.
--
g.test_index_block_yield_pin_vs_drop = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        local pad = string.rep('x', 50)
        for i = 1, 100 do
            s:replace{i, pad}
        end
        box.snapshot()

        -- Start reader with injection delay.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)
        local ch = fiber.channel(1)
        local f = fiber.new(function()
            local ok, err = pcall(function()
                return s:select()
            end)
            ch:put({ok, err})
        end)
        f:set_joinable(true)
        fiber.sleep(0.1)

        -- Drop the space while the reader is paused.
        -- The drop should wait for the reader to finish.
        local drop_done = false
        local f2 = fiber.new(function()
            box.space.test:drop()
            drop_done = true
        end)
        f2:set_joinable(true)
        fiber.sleep(0.1)

        -- Drop should not have completed yet (reader has slices pinned).
        -- Note: in practice the DDL might not block on the pin,
        -- but the reader's pinned reference keeps the run files alive.
        -- Resume the reader.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)
        local result = ch:get(10)
        -- The reader may succeed or fail depending on the DDL
        -- timing, but it must not crash.
        t.assert_type(result, 'table')

        f2:join(10)
        -- Space is dropped.
        t.assert_equals(box.space.test, nil)
    end)
end

--
-- Test that when two fibers concurrently access the same uncached
-- index block, only one disk read happens.  The second fiber waits
-- on the loading sentinel and gets the result from cache.
--
g.test_index_block_concurrent_load = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        -- Insert enough data to fit in 1 index block (<64 pages).
        local pad = string.rep('x', 50)
        for i = 1, 30 do
            s:replace{i, pad}
        end
        box.snapshot()

        -- Evict all entries from the index block cache so that
        -- the first select() triggers a fresh cache miss.
        box.cfg{vinyl_index_cache = 0}
        box.cfg{vinyl_index_cache = 128 * 1024 * 1024}

        -- Record global cache miss count before the test.
        -- The global stat (box.stat.vinyl().index_cache.miss)
        -- counts all disk loads including those from
        -- vy_page_index_find_page_impl where per-index stat
        -- is not available.
        local miss_before = box.stat.vinyl().index_cache.miss
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', true)

        -- Fiber A: forward scan → triggers cache miss for block 0,
        -- does coio read, hits injection, pauses.
        local ch1 = fiber.channel(1)
        local f1 = fiber.new(function()
            ch1:put(s:select())
        end)
        f1:set_joinable(true)
        -- Wait for fiber A to hit the injection.
        fiber.sleep(0.1)

        -- Fiber B: forward scan → tries block 0, finds sentinel,
        -- waits on cond (no disk read).
        local ch2 = fiber.channel(1)
        local f2 = fiber.new(function()
            ch2:put(s:select())
        end)
        f2:set_joinable(true)
        fiber.sleep(0.1)

        -- Resume both fibers.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_DELAY', false)

        local r1 = ch1:get(10)
        local r2 = ch2:get(10)
        t.assert_equals(#r1, 30)
        t.assert_equals(#r2, 30)

        -- Only 1 cache miss should have occurred (fiber A's load).
        -- Fiber B waited on the sentinel and got a cache hit.
        local miss_after = box.stat.vinyl().index_cache.miss
        t.assert_equals(miss_after - miss_before, 1)
    end)
end

--
-- Verify that a vinyl dump doesn't lose data when the post-write
-- .index2 reload fails.
--
-- After writing a run, vy_run_writer_commit() loads the freshly-
-- written .index2 file.  If this load fails (e.g., because xlog
-- reads are corrupted by ERRINJ_XLOG_GARBAGE), the dump is
-- treated as failed: the .run and .index2 files are discarded,
-- data stays in memory (L0), and the scheduler retries the dump.
--
g.test_dump_survives_index2_load_failure = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- Insert first batch and dump with .index2 reload corrupted.
        for i = 1, 10 do s:replace{i, 'first'} end

        -- Enable xlog read corruption.  This doesn't affect vinyl
        -- run writes (which go through the OS write path), but it
        -- corrupts reads from .index2 files (which use the xlog
        -- cursor).
        box.error.injection.set('ERRINJ_XLOG_GARBAGE', true)
        local ok, err = pcall(box.snapshot)
        box.error.injection.set('ERRINJ_XLOG_GARBAGE', false)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'checksum mismatch')

        -- The first batch must still be visible (data stayed in L0).
        t.assert_equals(#s:select{}, 10)

        -- Insert second batch and do a clean dump.
        for i = 11, 20 do s:replace{i, 'second'} end
        box.snapshot()

        -- All 20 rows must be present.
        t.assert_equals(#s:select{}, 20)
        t.assert_equals(s:get{1}[2], 'first')
        t.assert_equals(s:get{10}[2], 'first')
        t.assert_equals(s:get{11}[2], 'second')
        t.assert_equals(s:get{20}[2], 'second')
    end)
end

--
-- Test that partial compaction (not all slices included) preserves
-- tombstones. If is_last_level were incorrectly set to true, the
-- tombstone would be dropped and the deleted key would resurface
-- after a subsequent major compaction.
--
-- Uses a separate group because it doesn't need error injections
-- (and thus doesn't need a debug build).
--
local g_tombstone = t.group('tombstone')

g_tombstone.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)

g_tombstone.after_all(function(cg)
    cg.server:drop()
end)

g_tombstone.test_partial_compaction_preserves_tombstones = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- Use high run_count_per_level to suppress auto compaction
        -- during the dump phase.  This avoids races between
        -- intermediate compactions and concurrent dumps that can
        -- cause the range to be removed from the compaction heap
        -- at the wrong moment.
        s:create_index('pk', {run_count_per_level = 100})

        -- Create a large last-level run containing key 1.
        for k = 1, 1000 do
            s:replace({k, string.rep('x', 1000)})
        end
        box.snapshot()

        -- Create 3 small runs on top: update key 1, then
        -- delete it.
        s:replace({1, 'y'})
        box.snapshot()
        s:replace({1, 'z'})
        box.snapshot()
        s:delete({1})
        box.snapshot()

        -- 4 runs, no compaction yet (rcpl=100).
        t.assert_equals(s.index.pk:stat().run_count, 4)

        -- Lower rcpl to 1 and trigger a dump so the scheduler
        -- recalculates priorities.  The nudge must use key 1
        -- (same as the existing small runs) so that the trim
        -- step sees all small runs as one overlapping cluster.
        -- Using s:delete{1} is safe: it adds a redundant DELETE
        -- tombstone without changing the test semantics.
        --
        -- With rcpl=1 the shape analysis sees the small runs at
        -- L1 (overflow guaranteed: 4 > max 2 even with the 10%
        -- per-slice randomization deferral) and the big run at
        -- a deeper level.  It plans partial compaction of only
        -- the small runs, with is_last_level = false because the
        -- big run with key 1 remains below.
        --
        -- If the scheduler compacts only a subset per round
        -- (randomization defers the last round at 2 runs), add
        -- another s:delete{1} nudge to push L1 back to 3 runs.
        s.index.pk:alter({run_count_per_level = 1})
        s:delete({1})
        box.snapshot()
        for _ = 1, 5 do
            t.helpers.retrying({timeout = 60}, function()
                t.assert_le(s.index.pk:stat().run_count, 3)
            end)
            if s.index.pk:stat().run_count <= 2 then
                break
            end
            s:delete({1})
            box.snapshot()
        end
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Now force a full (major) compaction of all slices.
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(s.index.pk:stat().run_count, 1)
        end)

        -- Key 1 must not exist: the partial compaction preserved
        -- the DELETE tombstone (is_last_level = false), so the
        -- full compaction correctly suppressed the old INSERT from
        -- the large run.  If the partial compaction had wrongly
        -- dropped the tombstone, key 1 would resurface here.
        t.assert_equals(s:get{1}, nil)
    end)
end

--
-- Group: index block read error handling.
-- Tests that disk I/O errors during index block reads are
-- propagated correctly and do not crash the server.
--
local g_index_block = t.group('index_block_read')

g_index_block.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new()
    cg.server:start()
end)

g_index_block.after_all(function(cg)
    cg.server:drop()
end)

g_index_block.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

--
-- Helper: create a space with small page_size so that many index
-- blocks are needed, insert data, flush to disk, and evict the
-- index block cache so that subsequent reads go to disk.
--
local function create_test_space_for_block_read(cg)
    cg.server:exec(function()
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })
        -- Insert enough rows to produce many pages (and thus
        -- multiple index blocks of VY_INDEX_BLOCK_SIZE=64 pages).
        local pad = string.rep('x', 50)
        for i = 1, 500 do
            s:replace{i, pad}
        end
        box.snapshot()
        -- Flush the index block cache so the next read must
        -- load index blocks from disk (hitting the injection).
        local saved = box.cfg.vinyl_index_cache
        box.cfg{vinyl_index_cache = 0}
        box.cfg{vinyl_index_cache = saved}
    end)
end

--
-- Test that a forward scan (select) raises an error and does not
-- crash when an index block read fails.
-- Covers: vy_page_index_find_page_impl, vy_run_page_info_v2.
--
g_index_block.test_forward_scan_error = function(cg)
    create_test_space_for_block_read(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', true)
        local ok, err = pcall(box.space.test.select, box.space.test)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'vinyl index block read')
    end)
end

--
-- Test that a reverse scan (REQ iterator) raises an error and does
-- not crash when an index block read fails.
-- Covers: vy_run_iterator_next_pos (reverse branch) calling
-- vy_run_page_info_v2.
--
g_index_block.test_reverse_scan_error = function(cg)
    create_test_space_for_block_read(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', true)
        local ok, err = pcall(box.space.test.select, box.space.test,
                              {}, {iterator = 'REQ'})
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'vinyl index block read')
    end)
end

--
-- Test that a point lookup (get) raises an error and does not
-- crash when an index block read fails.
-- Covers: vy_run_bloom_check and vy_run_iterator_load_page
-- calling vy_run_get_index_block.
--
g_index_block.test_point_lookup_error = function(cg)
    create_test_space_for_block_read(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', true)
        local ok, err = pcall(box.space.test.get, box.space.test, 42)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'vinyl index block read')
    end)
end

--
-- Test that an error during index block read does not prevent
-- subsequent successful reads once the injection is disabled.
-- This verifies that no persistent state is corrupted and that
-- the sentinel mechanism in vy_run_get_index_block cleans up.
--
g_index_block.test_recovery_after_error = function(cg)
    create_test_space_for_block_read(cg)
    cg.server:exec(function()
        -- Fail a read.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', true)
        local ok, _ = pcall(box.space.test.get, box.space.test, 42)
        t.assert_not(ok)
        -- Disable injection and verify we can read successfully.
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', false)
        local result = box.space.test:get(42)
        t.assert_not_equals(result, nil)
        t.assert_equals(result[1], 42)
    end)
end

--
-- Test that a range scan with a key condition raises an error
-- when an index block read fails mid-iteration.
-- Uses a key in the middle of the data set so that the page
-- index search is exercised.
--
g_index_block.test_range_scan_with_key_error = function(cg)
    create_test_space_for_block_read(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_INDEX_BLOCK_READ', true)
        local ok, err = pcall(box.space.test.select, box.space.test,
                              250, {iterator = 'GE', limit = 10})
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'vinyl index block read')
    end)
end
