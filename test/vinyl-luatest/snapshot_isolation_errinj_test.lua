--
-- Error injection tests for SNAPSHOT isolation.
-- Debug-only: uses box.error.injection.
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group('snapshot_isolation_errinj')

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({
        box_cfg = {txn_isolation = 'snapshot'},
    })
    cg.server:start()
    cg.vinyl_cache = cg.server:exec(function()
        return box.cfg.vinyl_cache
    end)
end)

g.after_all(function(cg)
    if cg.server ~= nil then
        cg.server:drop()
    end
end)

g.after_each(function(cg)
    cg.server:exec(function(vinyl_cache)
        box.error.injection.set(
            'ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        box.error.injection.set(
            'ERRINJ_VY_READ_PAGE_DELAY', false)
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        box.error.injection.set('ERRINJ_WAL_IO', false)
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', false)
        -- Tests lower vinyl_cache (often to zero, disabling the
        -- cache); restore the default so later tests exercise
        -- the cache.
        box.cfg{vinyl_cache = vinyl_cache}
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end, {cg.vinyl_cache})
end)

-- WAL delay: a concurrent TX is prepared but not yet committed.
-- A read-only snapshot TX sees its data (is_prepared_ok = true)
-- but must wait for the prepared TX to commit before its own
-- commit can succeed. This prevents returning rolled-back data
-- to the user if the prepared TX fails WAL.
g.test_errinj_wal_delay_prepared_visible = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- TX1 writes key 1 with WAL delayed -- it will be
        -- "prepared" but not "committed".
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'prepared'}
            ch:put(true)
        end)

        -- TX2 (read-only snapshot) sees the prepared value.
        box.begin()
        t.assert_equals(s:get{1}, {1, 'prepared'})

        -- TX2's commit must block until TX1's WAL write
        -- completes (the read view has a pseudo-LSN).
        -- Release WAL from a separate fiber so TX2 can
        -- unblock.
        fiber.create(function()
            fiber.sleep(0.01)
            box.error.injection.set('ERRINJ_WAL_DELAY', false)
        end)

        box.commit()
        ch:get()

        -- After both committed, the write is visible.
        t.assert_equals(s:get{1}, {1, 'prepared'})
    end)
end

-- A read-only snapshot TX that saw prepared data must be
-- aborted if the prepared TX fails WAL. The read-only TX
-- blocks in commit (waiting for vlsn conversion), and the
-- cascading abort kills it.
g.test_errinj_readonly_tx_aborted_on_wal_failure = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- TX_A prepares, WAL delayed.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {1, 'doomed'})
            ch_a:put(ok)
        end)

        -- Read-only TX sees prepared data.
        box.begin()
        t.assert_equals(s:get(1), {1, 'doomed'})

        -- Fail TX_A's WAL from a separate fiber.
        fiber.create(function()
            fiber.sleep(0.01)
            box.error.injection.set('ERRINJ_WAL_WRITE_DISK', true)
            box.error.injection.set('ERRINJ_WAL_DELAY', false)
        end)

        -- TX_reader's commit blocks (pseudo-LSN read view),
        -- TX_A fails WAL, cascading abort kills TX_reader.
        local ok = pcall(box.commit)
        if not ok then pcall(box.rollback) end

        local tx_a_ok = ch_a:get()
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', false)

        t.assert_not(tx_a_ok, 'TX_A must fail WAL')
        t.assert_not(ok, 'read-only TX must be aborted '
                         .. 'after WAL failure')
        t.assert_equals(s:get(1), {1, 'init'})
    end)
end

-- WAL write failure: concurrent TX's write fails and is
-- rolled back. SNAPSHOT TX writing a different key should
-- not be affected.
g.test_errinj_wal_io_rollback = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        box.begin()
        s:replace{2, 'snapshot_write'}

        -- Concurrent TX writes a different key but WAL fails.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.error.injection.set('ERRINJ_WAL_IO', true)
            local ok = pcall(s.replace, s, {3, 'will_fail'})
            box.error.injection.set('ERRINJ_WAL_IO', false)
            ch:put(ok)
        end)
        local tx2_ok = ch:get()
        t.assert_not(tx2_ok, 'TX2 should fail (WAL error)')

        -- SNAPSHOT TX is unaffected and can commit.
        box.commit()

        t.assert_equals(s:get{1}, {1, 'original'})
        t.assert_equals(s:get{2}, {2, 'snapshot_write'})
        t.assert_equals(s:get{3}, nil) -- TX2 rolled back
    end)
end

-- Slow page read: TX1 pins a read view and starts a disk
-- read that blocks on page delay. While TX1 is blocked,
-- TX2 writes the same key and commits. The page delay is
-- then released. TX1 must see the original value because
-- its read view predates TX2's commit.
--
-- An orchestrator fiber coordinates the steps:
-- 1) TX1 pins vlsn, starts s:get{1} which blocks on page read
-- 2) TX2 commits s:replace{1, 'updated'}
-- 3) Page delay is released, TX1's read completes
-- 4) TX1 sees the snapshot value, not TX2's write
g.test_errinj_read_page_delay = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'on_disk'}
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)

        -- TX1: pin vlsn, then read key 1 (blocks on page delay).
        local result_ch = fiber.channel(1)
        local f1 = fiber.create(function()
            box.begin()
            s:get{999} -- pin the read view
            local val = s:get{1} -- blocks on page delay
            local ok = pcall(box.commit)
            result_ch:put({val = val, ok = ok})
        end)
        f1:set_joinable(true)

        -- TX2: write and commit while TX1 is blocked.
        local f2_ch = fiber.channel(1)
        local f2 = fiber.create(function()
            s:replace{1, 'updated'}
            f2_ch:put(true)
        end)
        f2:set_joinable(true)
        f2_ch:get()
        f2:join()

        -- Release page delay. TX1 resumes and completes.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        local result = result_ch:get()
        f1:join()

        t.assert(result.ok)
        t.assert_equals(result.val, {1, 'on_disk'},
            'snapshot TX must see original value despite '
            .. 'concurrent write during page read delay')

        -- After TX1 commits, the update is visible.
        t.assert_equals(s:get{1}, {1, 'updated'})
    end)
end

-- Write-write conflict with a preparing TX that is stuck in
-- WAL flight. TX_A prepares and hangs on WAL. TX_B starts,
-- writes the same key, and prepares. The conflict check must
-- detect TX_A (prepared but not yet committed) because its
-- future commit will be invisible to TX_B.
g.test_errinj_ww_conflict_with_preparing_tx = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- TX_A prepares and hangs on WAL.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'tx_a'}
            ch_a:put(true)
        end)

        -- TX_B reads to pin vlsn. With is_prepared_ok=true,
        -- the read view includes TX_A's prepared data.
        -- TX_B sees TX_A's write and can proceed without
        -- conflict (the prepared data is part of the snapshot).
        box.begin()
        t.assert_equals(s:get(1), {1, 'tx_a'})
        s:replace{1, 'tx_b'}

        -- Release WAL, let TX_A commit first.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        ch_a:get()

        -- TX_B commits on top of TX_A.
        local ok = pcall(box.commit)
        t.assert(ok, 'TX_B sees TX_A prepared data, no conflict')
        t.assert_equals(s:get(1), {1, 'tx_b'})
    end)
end

-- Read views must not leak when a snapshot TX that was sent
-- to a read view prepares concurrently with another TX.
--
-- The leak scenario:
-- 1. TX_A (snapshot) reads key K, gets sent to a read view
--    when a concurrent TX writes the same key
-- 2. TX_A prepares and yields for WAL write
-- 3. TX_B also prepares and yields for WAL write
-- 4. A reader encounters TX_B's prepared data, which triggers
--    creation of a shared read view that overwrites TX_A's
--    original read view
-- 5. If the original read view is not properly released,
--    it leaks
g.test_read_view_leak = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        t.assert_equals(box.stat.vinyl().tx.read_views, 0)

        for i = 1, 20 do
            -- TX_A (snapshot) reads key 1, gets sent to a read
            -- view by a concurrent write.
            box.begin()
            s:get{1}
            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace{1, 'conflict_' .. i}
                ch:put(true)
            end)
            ch:get()
            -- TX_A writes a key so it goes through prepare.
            s:replace{1000 + i, 'data_a'}

            -- TX_B is a simple write TX (no read view).
            -- Both TX_A and TX_B will prepare and wait for WAL.
            box.error.injection.set('ERRINJ_WAL_DELAY', true)

            -- TX_B prepares in a fiber (stuck on WAL delay).
            local fb_done = fiber.channel(1)
            fiber.new(function()
                s:replace{2000 + i, 'data_b'}
                fb_done:put(true)
            end)

            -- Reader runs after both TX_A and TX_B are
            -- prepared. It reads key (2000+i) which has TX_B's
            -- prepared data, triggering a shared read view that
            -- overwrites TX_A's original read view.
            local fr_done = fiber.channel(1)
            fiber.new(function()
                s:get{2000 + i}
                box.error.injection.set('ERRINJ_WAL_DELAY', false)
                fr_done:put(true)
            end)

            -- TX_A commits (prepare is synchronous, then yields
            -- on WAL delay). TX_B and reader run during yield.
            box.commit()
            fb_done:get()
            fr_done:get()
        end

        local rv = box.stat.vinyl().tx.read_views
        t.assert_equals(rv, 0,
            ('read views leaked: %d (expected 0)'):format(rv))
    end)
end

-- With lazy vlsn and prepared visibility, the read view
-- includes prepared TXs at the time of the first read.
-- Writer A prepares before the reader. Writer B prepares
-- after. The reader sees Writer A's data but not Writer B's.
g.test_deferred_read_view_with_prepared_tx = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        s:replace{2, 'original'}
        box.snapshot()

        -- Writer A: prepare but hang on WAL.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'updated_a'}
            ch_a:put(true)
        end)

        -- Reader: pin vlsn. Writer A is prepared, so the
        -- read view includes it (is_prepared_ok = true).
        box.begin()
        t.assert_equals(s:get(2), {2, 'original'})

        -- Writer B: prepares AFTER the reader's vlsn.
        local ch_b = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'updated_b'}
            ch_b:put(true)
        end)

        -- Release WAL, let both writers commit.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        ch_a:get()
        ch_b:get()

        -- Key 1: Writer A prepared before our vlsn → visible.
        t.assert_equals(s:get(1), {1, 'updated_a'})
        -- Key 2: Writer B prepared after our vlsn → invisible.
        t.assert_equals(s:get(2), {2, 'original'})
        box.rollback()
    end)
end

-- A snapshot TX scanning with s:pairs() yields on a page read.
-- While it is blocked, a writer commits a blind update to key 2.
-- The snapshot TX still sees the original data because its
-- read view predates the concurrent write.
-- A snapshot TX scanning with s:pairs() yields on a page read.
-- While it is blocked, a writer commits. The scan must see
-- the snapshot, not the concurrent write.
-- Orchestrator: TX1 pins vlsn, starts scan (blocks on page
-- delay), TX2 commits, delay released, scan completes.
g.test_read_page_delay_scan_sees_snapshot = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        s:replace{2, 100}
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)

        -- TX1: pin vlsn, scan (blocks on page delay).
        local result_ch = fiber.channel(1)
        local f1 = fiber.create(function()
            box.begin()
            s:get{999} -- pin vlsn
            local total = 0
            for _, tuple in s:pairs() do
                total = total + tuple[2]
            end
            box.rollback()
            result_ch:put(total)
        end)
        f1:set_joinable(true)

        -- TX2: concurrent write while TX1 is blocked.
        local f2_ch = fiber.channel(1)
        local f2 = fiber.create(function()
            s:replace{2, 999}
            f2_ch:put(true)
        end)
        f2:set_joinable(true)
        f2_ch:get()
        f2:join()

        -- Release delay, let TX1 finish.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        local total = result_ch:get()
        f1:join()

        t.assert_equals(total, 200, 'snapshot scan sees ' ..
            'original data (got ' .. total .. ')')
    end)
end

-- Read-before-write must see prepared data. TX_A prepares
-- and hangs on WAL (data in vy_mem with pseudo-LSN).
-- TX_B (outside the conflict window) writes the same key.
-- The read-before-write check must see TX_A's prepared
-- data and detect the conflict.
g.test_errinj_read_before_write_sees_prepared = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        s:replace{1000, 'pin'}

        -- TX_A prepares and hangs on WAL. Its data is in
        -- vy_mem with a pseudo-LSN.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 999}
            ch_a:put(true)
        end)

        -- TX_B writes key 1. The conflict with TX_A's
        -- prepared entry is detected at prepare time via
        -- the BPS tree older entry (pseudo-LSN > vlsn).
        local ok_write = pcall(s.replace, s, {1, 110})
        local ok_commit = true
        if ok_write then
            box.error.injection.set('ERRINJ_WAL_DELAY', false)
            ch_a:get()
            ch_a = nil
            ok_commit = pcall(box.commit)
        end
        if ch_a then
            box.error.injection.set('ERRINJ_WAL_DELAY', false)
            ch_a:get()
        end
        if not ok_write or not ok_commit then
            pcall(box.rollback)
        end

        t.assert(not ok_write or not ok_commit,
            'conflict with prepared TX must be detected')
    end)
end

-- A slow-path read-before-write that yields on disk must fail
-- immediately if the space is dropped while it is waiting.
g.test_errinj_slow_path_space_drop_aborts_dml = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {bloom_fpr = 1})
        s:replace{1, 'seed'}
        s:replace{2, 'old'}
        box.snapshot()

        local ready_ch = fiber.channel(1)
        local go_ch = fiber.channel(1)
        local done_ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:get(1) -- pin vlsn
            s:replace{1000, 'pin'}
            ready_ch:put(true)
            go_ch:get()
            local ok_write, err_write = pcall(s.replace, s, {2, 'new'})
            local res = {ok_write = ok_write}
            if not ok_write then
                res.err_write = tostring(err_write)
                pcall(box.rollback)
                done_ch:put(res)
                return
            end
            local ok_commit, err_commit = pcall(box.commit)
            res.ok_commit = ok_commit
            res.err_commit = err_commit ~= nil and tostring(err_commit) or nil
            if not ok_commit then
                pcall(box.rollback)
            end
            done_ch:put(res)
        end)
        ready_ch:get()

        -- Write a newer version of key 2.
        do
            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace{2, 'concurrent'}
                ch:put(true)
            end)
            ch:get()
        end
        -- Dump to move the concurrent write to disk and
        -- trigger vy_mem rotation (generation change).
        -- This makes the TX "slow" (vlsn < min_lsn).
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        go_ch:put(true)

        t.assert_equals(done_ch:get(0.1), nil,
            'replace must block in the slow path')

        s:drop()
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)

        local res = done_ch:get()
        t.assert_not(res.ok_write,
            'slow-path DML must fail immediately after space drop')
        t.assert_str_contains(res.err_write,
            'Transaction has been aborted')
        t.assert_equals(box.space.test, nil)
    end)
end

-- vy_tx_manager_check_concurrent_write calls vy_lsm_check_concurrent_write
-- which yields on disk reads. If the space is dropped during
-- the yield, the LSM must stay alive (ref-counted).
-- Regression test: verify no crash (use-after-free).
g.test_errinj_slow_path_lsm_ref_during_yield = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {bloom_fpr = 1})
        s:replace{1, 'old'}
        box.snapshot()

        -- Snapshot TX starts, reads to pin vlsn.
        local ready_ch = fiber.channel(1)
        local go_ch = fiber.channel(1)
        local done_ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:get(1) -- pin vlsn
            s:replace{1000, 'pin'}
            ready_ch:put(true)
            go_ch:get()
            local ok, err = pcall(s.replace, s, {1, 'new'})
            pcall(box.rollback)
            done_ch:put({ok = ok, err = tostring(err)})
        end)
        ready_ch:get()

        -- Write a newer version so the slow path has
        -- something to find on disk.
        do
            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace{1, 'concurrent'}
                ch:put(true)
            end)
            ch:get()
        end

        -- Dump to move the concurrent write to disk and
        -- trigger vy_mem rotation (generation change).
        box.snapshot()

        -- Yield inside vy_lsm_check_concurrent_write (the slow path)
        -- after pinning slices but before scanning them.
        box.error.injection.set(
            'ERRINJ_VY_POINT_LOOKUP_LSN_DELAY', true)
        go_ch:put(true)

        -- The worker runs until it yields inside
        -- vy_lsm_check_concurrent_write. Drop the space while the
        -- LSM has no extra ref.
        t.assert_equals(done_ch:get(0.1), nil,
            'DML must block in the slow path')
        s:drop()

        box.error.injection.set(
            'ERRINJ_VY_POINT_LOOKUP_LSN_DELAY', false)

        -- The fiber should complete without crashing
        -- (use-after-free on the freed LSM).
        local res = done_ch:get()
        t.assert_not(res.ok, 'DML must fail after space drop')
    end)
end

--
-- Test for the race between the DML-time concurrent write
-- check and the dump-completion WW check. TX_A's DML pauses
-- inside vy_lsm_check_concurrent_write (via error injection).
-- A helper fiber completes a dump while TX_A is paused. The
-- dump-completion WW check scans TX_A's write set. With the
-- fix (check moved into vy_tx_set), the key IS in the write
-- set and the conflict is detected. Without the fix, the
-- check ran before vy_tx_set and the key was missing.
--
g.test_race_dml_check_vs_dump_completion = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, vlsn before TX_B's write.
        box.begin()
        s:get(1)

        -- TX_B: concurrent write to the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 200}
            ch:put(true)
        end)
        ch:get()

        -- Delay the dump worker and start a dump so it's
        -- queued but not yet complete.
        box.error.injection.set('ERRINJ_VY_DUMP_DELAY', true)
        local snap_done = fiber.channel(1)
        fiber.create(function()
            box.snapshot()
            snap_done:put(true)
        end)
        t.helpers.retrying({timeout = 5}, function()
            t.assert_gt(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- Arm the gate and set up a helper that will
        -- complete the dump while the main fiber is paused.
        box.error.injection.set(
            'ERRINJ_VY_CHECK_CONCURRENT_WRITE_GATE', 1)
        local dump_count =
            box.stat.vinyl().scheduler.dump_count
        fiber.create(function()
            -- Wait for the main fiber to reach the gate.
            t.helpers.retrying({timeout = 5}, function()
                t.assert_gt(box.error.injection.get(
                    'ERRINJ_VY_CHECK_CONCURRENT_WRITE_GATE'), 1)
            end)
            -- Release the dump. It completes while the
            -- main fiber is paused at the gate.
            box.error.injection.set('ERRINJ_VY_DUMP_DELAY', false)
            t.helpers.retrying({timeout = 5}, function()
                t.assert_gt(
                    box.stat.vinyl().scheduler.dump_count,
                    dump_count)
            end)
            -- Release the gate.
            box.error.injection.set(
                'ERRINJ_VY_CHECK_CONCURRENT_WRITE_GATE', 0)
        end)

        -- TX_A writes the same key. This enters
        -- vy_lsm_check_concurrent_write and hits the gate.
        -- The helper fiber completes the dump while we're
        -- paused, then releases the gate. The conflict is
        -- detected either at DML time (dump-completion WW
        -- check aborts the TX while paused at the gate) or
        -- at commit time.
        local ok = pcall(s.replace, s, {1, 300})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end

        snap_done:get()

        t.assert_not(ok, 'WW conflict must be detected: ' ..
                     tostring(s:get(1)))
    end)
end

--
-- Test that the PK concurrent write check inside vy_tx_set
-- catches a conflict on disk when a dump completes during
-- vy_get (old-tuple lookup, before vy_tx_set). The vy_get
-- is a read (not a write), so the PK key is NOT in the
-- write set during the yield. After vy_get returns,
-- vy_tx_set adds the PK key and vy_lsm_check_concurrent_write
-- falls through to the disk scan via lsm->dump_lsn.
--
g.test_pk_conflict_on_disk_after_dump = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        -- Need a second index to trigger vy_get for old-tuple
        -- lookup (defer_deletes is false by default).
        s:create_index('sk', {parts = {2, 'unsigned'},
                              unique = false})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, vlsn before TX_B's write.
        box.begin()
        s:get(1)

        -- TX_B: concurrent write to the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 200}
            ch:put(true)
        end)
        ch:get()

        -- Delay the dump worker and start a dump.
        box.error.injection.set('ERRINJ_VY_DUMP_DELAY', true)
        local snap_done = fiber.channel(1)
        fiber.create(function()
            box.snapshot()
            snap_done:put(true)
        end)
        t.helpers.retrying({timeout = 5}, function()
            t.assert_gt(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- Pause vy_point_lookup (used by vy_get for old-tuple
        -- lookup). The main fiber will pause here during
        -- s:replace, BEFORE vy_tx_set adds the PK key.
        box.error.injection.set(
            'ERRINJ_VY_POINT_LOOKUP_DELAY', true)

        local dump_count =
            box.stat.vinyl().scheduler.dump_count
        fiber.create(function()
            -- Wait for the main fiber to reach the gate.
            fiber.sleep(0.1)
            -- Release the dump. It completes while the
            -- main fiber is in vy_get (PK key NOT in
            -- write set). dump-completion WW check misses it.
            box.error.injection.set(
                'ERRINJ_VY_DUMP_DELAY', false)
            t.helpers.retrying({timeout = 5}, function()
                t.assert_gt(
                    box.stat.vinyl().scheduler.dump_count,
                    dump_count)
            end)
            -- Release the point lookup gate.
            box.error.injection.set(
                'ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        end)

        -- TX_A writes the same key. vy_get for old-tuple
        -- lookup hits the gate. The helper completes the
        -- dump, then releases the gate. After vy_get
        -- returns, vy_tx_set adds the PK key and
        -- vy_lsm_check_concurrent_write must detect the
        -- conflict on disk.
        local ok = pcall(s.replace, s, {1, 300})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end

        snap_done:get()

        t.assert_not(ok, 'PK conflict on disk must be '
                     .. 'detected: ' .. tostring(s:get(1)))
    end)
end

-- Unique SK conflict in a sealed mem between entries with
-- different PKs. TX_B inserts {2, 200} (SK=200, PK=2), then
-- a dump is started but delayed so the entry stays in a sealed
-- mem. TX_A inserts {3, 200} (SK=200, PK=3). The DML-time
-- sealed mem scan must detect the conflict despite different
-- PK values.
g.test_unique_sk_conflict_sealed_mem_different_pk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, vlsn before TX_B.
        box.begin()
        s:get(1)

        -- TX_B: insert with SK=200, PK=2.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, 200}
            ch:put(true)
        end)
        ch:get()

        -- Start a dump but delay it so TX_B's entry stays
        -- in a sealed mem.
        box.error.injection.set('ERRINJ_VY_DUMP_DELAY', true)
        local snap_done = fiber.channel(1)
        fiber.create(function()
            box.snapshot()
            snap_done:put(true)
        end)
        t.helpers.retrying({timeout = 5}, function()
            t.assert_gt(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- TX_A: insert with SK=200, PK=3. TX_B's {200, 2}
        -- is in a sealed mem.
        local ok = pcall(s.insert, s, {3, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end

        box.error.injection.set('ERRINJ_VY_DUMP_DELAY', false)
        snap_done:get()

        t.assert_not(ok, 'unique SK conflict in sealed mem '
                     .. 'must be detected with different PKs')
    end)
end

-- Unique SK conflict detected at dump completion between
-- entries with different PKs. TX_A inserts {3, 200} (SK=200,
-- PK=3) while no conflict exists. Then TX_B inserts {2, 200}
-- (SK=200, PK=2) and commits. A dump moves TX_B's entry to a
-- sealed mem. The dump-completion WW check must detect the
-- conflict in TX_A's write set despite different PK values.
g.test_unique_sk_conflict_dump_completion_different_pk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, inserts {3, 200} first (no
        -- conflict yet).
        box.begin()
        s:get(1)
        s:insert{3, 200}

        -- TX_B: insert with same SK=200, different PK=2.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, 200}
            ch:put(true)
        end)
        ch:get()

        -- Dump. TX_B's entry goes to a sealed mem and gets
        -- dumped. The dump-completion WW check scans TX_A's
        -- write set (which has SK entry {200, 3}) against
        -- the sealed mem (which has {200, 2}).
        box.snapshot()

        -- TX_A should have been aborted by the dump-completion
        -- check.
        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'unique SK conflict at dump completion '
                     .. 'must be detected with different PKs')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- A snapshot TX scans while a concurrent writer modifies a
-- key in the scan range. The writer's vy_cache_on_write
-- invalidates the cache entry and adjusts chain links.
-- The cache must remain consistent: a subsequent autocommit
-- scan must return correct results.
g.test_scan_cache_with_concurrent_write = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 5 do
            s:replace{i, i * 100}
        end
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)

        local scan_result
        local scan_done = fiber.channel(1)

        fiber.create(function()
            box.begin()
            scan_result = s:select()
            box.commit()
            scan_done:put(true)
        end)

        fiber.sleep(0.01)
        s:replace{3, 999}

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        scan_done:get()

        t.assert_equals(scan_result, {
            {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500}
        })

        local result = s:select()
        t.assert_equals(result, {
            {1, 100}, {2, 200}, {3, 999}, {4, 400}, {5, 500}
        }, 'autocommit scan must see the latest data')
    end)
end

-- Snapshot scan where a concurrent writer inserts new keys
-- in the gaps of the scan range. The inserted keys are
-- invisible to the snapshot TX but must be visible to
-- subsequent autocommit scans.
g.test_scan_cache_with_concurrent_insert = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        s:replace{3, 300}
        s:replace{5, 500}
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)

        local scan_result
        local scan_done = fiber.channel(1)

        fiber.create(function()
            box.begin()
            scan_result = s:select()
            box.commit()
            scan_done:put(true)
        end)

        fiber.sleep(0.01)
        s:replace{2, 200}
        s:replace{4, 400}
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        scan_done:get()

        t.assert_equals(scan_result, {
            {1, 100}, {3, 300}, {5, 500}
        })

        local result = s:select()
        t.assert_equals(result, {
            {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500}
        }, 'inserted keys must not be hidden by cache')
    end)
end

-- Snapshot scan where a concurrent writer deletes a key
-- in the scan range. The delete is invisible to the snapshot
-- but must be visible to autocommit.
g.test_scan_cache_with_concurrent_delete = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 5 do
            s:replace{i, i * 100}
        end
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)

        local scan_result
        local scan_done = fiber.channel(1)

        fiber.create(function()
            box.begin()
            scan_result = s:select()
            box.commit()
            scan_done:put(true)
        end)

        fiber.sleep(0.01)
        s:delete{3}
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        scan_done:get()

        t.assert_equals(scan_result, {
            {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500}
        })

        local result = s:select()
        t.assert_equals(result, {
            {1, 100}, {2, 200}, {4, 400}, {5, 500}
        }, 'deleted key must not be served from cache')
    end)
end

-- ---------------------------------------------------------------
-- Prepared read view tests.
-- With lazy vlsn, a snapshot TX's read view is assigned at the
-- first read and includes all prepared TXs (is_prepared_ok=true).
-- ---------------------------------------------------------------

-- Prepared visibility boundary: a reader sees data from TXs
-- prepared before its read view, not from TXs prepared after.
g.test_prepared_visibility_boundary = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- TX_A prepares and hangs on WAL.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'prepared_a'}
            ch_a:put(true)
        end)

        -- TX_reader starts reading. With is_prepared_ok=true,
        -- its read view includes TX_A's prepared data.
        box.begin()
        local val = s:get(1)

        -- TX_B prepares AFTER TX_reader's read view.
        local ch_b = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'prepared_b'}
            ch_b:put(true)
        end)

        -- TX_reader must see TX_A's data but NOT TX_B's.
        t.assert_equals(val, {1, 'prepared_a'},
            'reader must see data prepared before its read view')
        t.assert_equals(s:get(2), nil,
            'reader must not see data prepared after its read view')

        -- Release WAL, let both commit.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        ch_a:get()
        ch_b:get()

        -- Repeatable read: re-read must see the same snapshot.
        t.assert_equals(s:get(1), {1, 'prepared_a'})
        t.assert_equals(s:get(2), nil)
        box.rollback()
    end)
end

-- Repeatable read across pseudo-LSN -> real LSN conversion.
-- TX_A prepares (pseudo-LSN), reader pins read view, TX_A
-- commits (pseudo-LSN converted to real LSN). Reader re-reads
-- and must see the same data.
g.test_prepared_repeatable_read_across_commit = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- TX_A prepares and hangs on WAL.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'from_a'}
            ch_a:put(true)
        end)

        -- Reader pins read view at TX_A's pseudo-LSN.
        box.begin()
        t.assert_equals(s:get(1), {1, 'from_a'})

        -- TX_A commits (pseudo-LSN -> real LSN).
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        ch_a:get()

        -- Another TX commits after TX_A.
        local ch_c = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'from_c'}
            ch_c:put(true)
        end)
        ch_c:get()

        -- Repeatable read: must still see TX_A's data,
        -- not TX_C's (committed after our read view).
        t.assert_equals(s:get(1), {1, 'from_a'})
        box.rollback()
    end)
end

-- WAL failure cascading abort. Accumulate several autocommit
-- writers in the WAL pipeline (delayed). Start snapshot readers
-- that see their prepared data. Then enable WAL IO error and
-- release the delay. The writers fail. The readers that saw
-- prepared data (vlsn >= MAX_LSN) must be aborted.
g.test_prepared_wal_failure_cascading_abort = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 5 do
            s:replace{i, 'init'}
        end
        box.snapshot()

        -- Accumulate writers in WAL delay.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local writers = {}
        for i = 1, 3 do
            local ch = fiber.channel(1)
            fiber.create(function()
                local ok = pcall(s.replace, s, {i, 'batch1_' .. i})
                ch:put(ok)
            end)
            writers[i] = ch
        end

        -- Readers that see the prepared batch. Use unbuffered
        -- channels for strict synchronization.
        local reader_vals = {}
        local reader_done = {}
        for i = 1, 2 do
            local val_ch = fiber.channel(0)
            local done_ch = fiber.channel(0)
            fiber.create(function()
                box.begin()
                local val = s:get(1)
                val_ch:put(val)
                done_ch:get()
                local ok = pcall(box.commit)
                if not ok then pcall(box.rollback) end
                done_ch:put(ok)
            end)
            reader_vals[i] = val_ch
            reader_done[i] = done_ch
        end

        -- Verify readers saw prepared data.
        for i = 1, 2 do
            local val = reader_vals[i]:get()
            t.assert_equals(val, {1, 'batch1_1'},
                'reader ' .. i .. ' must see prepared data')
        end

        -- Enable disk write error, then release delay.
        -- ERRINJ_WAL_WRITE_DISK fails in the WAL writer
        -- thread (xlog.c), after the delay, unlike
        -- ERRINJ_WAL_IO which is checked before the delay.
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', true)
        box.error.injection.set('ERRINJ_WAL_DELAY', false)

        -- Collect writer results -- all must fail.
        for i = 1, 3 do
            local ok = writers[i]:get()
            t.assert_not(ok, 'writer ' .. i .. ' must fail WAL')
        end
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', false)

        -- Release readers and check they were aborted.
        for i = 1, 2 do
            reader_done[i]:put('go')
            local ok = reader_done[i]:get()
            t.assert_not(ok,
                'reader ' .. i .. ' must be aborted '
                .. 'after WAL failure')
        end

        -- Original data intact.
        for i = 1, 5 do
            t.assert_equals(s:get(i), {i, 'init'})
        end
    end)
end

-- Over-abort regression. On WAL failure, vy_tx_rollback_after_prepare()
-- must abort only snapshot readers whose read view actually includes the
-- failing TX's pseudo-LSN (vlsn >= MAX_LSN + tx->psn), not every reader
-- pinned at any pseudo-LSN. A reader pins its read view at MAX_LSN +
-- (psn of the LAST prepared TX at first-read time) -- see
-- vy_tx_manager_read_view(). So a reader that pinned while only an
-- earlier TX (psn pA) was prepared must NOT be aborted when a later,
-- unrelated TX (psn pB > pA) fails its WAL.
--
-- Interleaving:
--   1. Writer A prepares key 10 @ pseudo-LSN pA, yields on WAL delay
--      (stays prepared for the whole scenario).
--   2. Snapshot reader S's first read pins its read view at MAX_LSN + pA
--      (A is the only prepared TX) -- S "saw" only A.
--   3. Writer B prepares key 20 @ pseudo-LSN pB > pA, then fails its WAL
--      synchronously via ERRINJ_WAL_IO (checked in the TX thread, before
--      the delayed WAL pipe, so A stays suspended). B's rollback runs the
--      cascading abort loop.
--   4. A's WAL is released and A commits successfully, converting S's
--      read view to a real LSN.
--   5. S commits. On the buggy build S was spuriously aborted (commit
--      fails); on the fixed build S survives and commits.
g.test_unrelated_reader_not_aborted_on_wal_failure = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'committed'}
        box.snapshot()

        -- (1) Writer A prepares (pseudo-LSN pA), yields on WAL delay.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local a_ch = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {10, 'A_prepared'})
            a_ch:put(ok)
        end)

        -- (2) Snapshot reader S pins read view at MAX_LSN + pA (saw only A).
        local to_s = fiber.channel(1)
        local from_s = fiber.channel(1)
        local s_commit_ok
        fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            s:get{1}
            from_s:put('pinned')
            to_s:get()
            s_commit_ok = pcall(box.commit)
            from_s:put('committed')
        end)
        t.assert_equals(from_s:get(), 'pinned')

        -- (3) Writer B prepares (pseudo-LSN pB > pA) and fails its WAL
        --     synchronously, without releasing A's delayed WAL.
        box.error.injection.set('ERRINJ_WAL_IO', true)
        local b_ok = pcall(s.replace, s, {20, 'B_doomed'})
        box.error.injection.set('ERRINJ_WAL_IO', false)
        t.assert_not(b_ok, 'writer B must fail its WAL')

        -- (4) Release A; it commits and converts S's read view to a real LSN.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        t.assert(a_ch:get(), 'writer A must commit')

        -- (5) S must NOT have been aborted by B's unrelated failure.
        to_s:put('go')
        t.assert_equals(from_s:get(), 'committed')
        t.assert(s_commit_ok, 'snapshot reader pinned at an earlier prepared '
                 .. 'TX must survive a later TX WAL failure')

        t.assert_equals(s:get{10}, {10, 'A_prepared'})
        t.assert_equals(s:get{20}, nil)
    end)
end

-- Companion sanity: a reader that DID see the failing TX must still be
-- aborted (guards against the fix under-aborting).
g.test_dependent_reader_still_aborted_on_wal_failure = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'committed'}
        box.snapshot()

        -- Single writer B prepares (pseudo-LSN pB), yields on WAL delay.
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local b_ch = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {10, 'B'})
            b_ch:put(ok)
        end)

        -- Reader S pins at MAX_LSN + pB (B is the last prepared TX) -> saw B.
        local to_s = fiber.channel(1)
        local from_s = fiber.channel(1)
        local s_commit_ok
        fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            s:get{1}
            from_s:put('pinned')
            to_s:get()
            s_commit_ok = pcall(box.commit)
            from_s:put('committed')
        end)
        t.assert_equals(from_s:get(), 'pinned')

        -- B fails its WAL via the delayed-pipe disk-write error.
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', true)
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        t.assert_not(b_ch:get(), 'writer B must fail its WAL')
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', false)

        -- S saw B's prepared data, which was rolled back: S must be aborted.
        to_s:put('go')
        t.assert_equals(from_s:get(), 'committed')
        t.assert_not(s_commit_ok, 'snapshot reader that saw the failing TX '
                     .. 'must be aborted')

        t.assert_equals(s:get{10}, nil)
    end)
end

-- A scan must not resurrect its last cached statement. The chain
-- link machinery re-inserts the previous node into the cache; if the
-- key was overwritten and invalidated between two iterator steps,
-- that re-insert brings a superseded value back as the latest. The
-- overwrite is invisible to the anchor check when the anchor was
-- read prepared: results built from in-memory statements are
-- duplicates whose pseudo-LSN is never confirmed in place, and no
-- real LSN exceeds a pseudo-LSN.
g.test_scan_does_not_resurrect_overwritten_anchor = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'v1'}
        s:replace{2, 'w'}

        -- Suspend {1,'v2'} prepared: the snapshot reader will read it
        -- and anchor its cache chain on the pseudo-LSN duplicate.
        local stmts = box.stat.vinyl().tx.statements
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_w = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {1, 'v2'})
            ch_w:put(ok)
        end)
        t.helpers.retrying({}, function()
            t.assert_gt(box.stat.vinyl().tx.statements, stmts)
        end)

        -- The reader iterates step by step: each step returns to this
        -- fiber, so the writes below land between the anchor and the
        -- next cached statement.
        box.begin({txn_isolation = 'snapshot'})
        local gen, param, state = s:pairs()
        local r1, r2, r3
        state, r1 = gen(param, state)

        -- The write commits and is overwritten; the invalidations
        -- purge key 1 from the cache.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        t.assert(ch_w:get(), 'writer must commit')
        local ch_w2 = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {1, 'v3'})
            ch_w2:put(ok)
        end)
        t.assert(ch_w2:get(), 'overwriter must commit')

        -- The next steps cache {2,'w'} chained to the anchor and the
        -- end-of-scan boundary.
        state, r2 = gen(param, state)
        r3 = select(2, gen(param, state))
        box.commit()
        t.assert_equals(r1, {1, 'v2'})
        t.assert_equals(r2, {2, 'w'})
        t.assert_equals(r3, nil)

        -- No reader may see a resurrected {1,'v2'}. A node brought
        -- back by the scan carries a pseudo LSN, so the vulnerable
        -- reader is one whose view also lies at a pseudo LSN: a
        -- snapshot transaction begun while any newer write sits
        -- prepared. It must be the first reader to touch the key: a
        -- read at a committed view would repair the node instead.
        stmts = box.stat.vinyl().tx.statements
        box.error.injection.set('ERRINJ_WAL_DELAY', true)
        local ch_w3 = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {5, 'z'})
            ch_w3:put(ok)
        end)
        t.helpers.retrying({}, function()
            t.assert_gt(box.stat.vinyl().tx.statements, stmts)
        end)
        box.begin({txn_isolation = 'snapshot'})
        local poisoned = s:get(1)
        -- The reader observed a prepared write, so its commit waits
        -- for the writer's WAL confirmation: release the WAL first.
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        box.commit()
        t.assert_equals(poisoned, {1, 'v3'})
        t.assert(ch_w3:get(), 'last writer must commit')

        t.assert_equals(s:get(1), {1, 'v3'})
        t.assert_equals(s:select(),
                        {{1, 'v3'}, {2, 'w'}, {5, 'z'}})
    end)
end
