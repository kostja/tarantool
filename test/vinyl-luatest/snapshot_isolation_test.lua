local server = require('luatest.server')
local t = require('luatest')

local g = t.group('snapshot_isolation')

g.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {
            memtx_use_mvcc_engine = true,
            txn_isolation = 'snapshot',
        },
    })
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
        if box.space.test_memtx ~= nil then
            box.space.test_memtx:drop()
        end
        if box.cfg.vinyl_cache == 0 then
            box.cfg{vinyl_cache = 1024 * 1024}
        end
        local rv = box.stat.vinyl().tx.read_views
        if rv > 0 then
            require('log').error('LEAK: %d read views after test', rv)
        end
    end)
end)

-- A pure-read SNAPSHOT TX (no DML, no engine TX) must still
-- see a consistent snapshot.  Use select (range scan) to
-- avoid the point lookup cache masking the issue.
g.test_repeatable_read_pure_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        box.begin()
        t.assert_equals(s:select(), {{1, 'original'}})

        -- Concurrent TX inserts key 2.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'phantom'}
            ch:put(true)
        end)
        ch:get()

        -- Under snapshot isolation, the phantom must not appear.
        t.assert_equals(s:select(), {{1, 'original'}})
        box.rollback()
    end)
end

-- SNAPSHOT TX must see a consistent point-in-time snapshot:
-- reading the same key twice must return the same value even
-- if a concurrent TX modifies it in between.
g.test_repeatable_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        box.begin()
        -- First read.
        t.assert_equals(s:get(1), {1, 'original'})

        -- Concurrent TX modifies the key and commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'modified'}
            ch:put(true)
        end)
        ch:get()

        -- Second read must return the same value (snapshot).
        t.assert_equals(s:get(1), {1, 'original'})
        box.rollback()
    end)
end

-- Same as test_repeatable_read but for range scans:
-- a full scan must return the same result set even if
-- concurrent TXs insert or delete rows in between.
g.test_repeatable_scan = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{3, 'c'}
        box.snapshot()

        box.begin()
        -- First scan.
        local v1 = s:select()
        t.assert_equals(v1, {{1, 'a'}, {3, 'c'}})

        -- Concurrent TX inserts a new row.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'b'}
            ch:put(true)
        end)
        ch:get()

        -- Second scan must return the same result (snapshot).
        local v2 = s:select()
        t.assert_equals(v2, {{1, 'a'}, {3, 'c'}})
        box.rollback()
    end)
end

-- SNAPSHOT TX reads key A, concurrent TX writes key A,
-- SNAPSHOT TX writes key B and commits successfully.
g.test_no_rw_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        box.snapshot()

        box.begin()
        s:get(1)

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'b'}
            ch:put(true)
        end)
        ch:get()

        s:replace{2, 'c'}
        box.commit()

        t.assert_equals(s:get(1), {1, 'b'})
        t.assert_equals(s:get(2), {2, 'c'})
    end)
end

-- Two SNAPSHOT TXs write the same key. First to commit wins,
-- second gets ER_TRANSACTION_CONFLICT.
g.test_ww_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- TX1: snapshot, reads to pin vlsn, writes key 1.
        box.begin()
        s:get(1)
        s:replace{1, 'tx1'}

        -- TX2: snapshot, writes key 1 concurrently.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 'tx2'}
            box.commit()
            ch:put(true)
        end)

        -- TX2 commits first (it started after TX1 but commits before).
        ch:get()

        -- TX1 tries to commit and gets a conflict.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'Transaction has been aborted')

        t.assert_equals(s:get(1), {1, 'tx2'})
    end)
end

-- Two SNAPSHOT TXs write different keys. Both commit successfully.
g.test_no_ww_conflict_different_keys = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        box.begin()
        s:replace{1, 'tx1'}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{2, 'tx2'}
            box.commit()
            ch:put(true)
        end)

        ch:get()

        box.commit()

        t.assert_equals(s:get(1), {1, 'tx1'})
        t.assert_equals(s:get(2), {2, 'tx2'})
    end)
end

-- SNAPSHOT TX and BEST_EFFORT TX write the same key.
-- Write-write conflict detected for the SNAPSHOT TX.
g.test_ww_conflict_with_best_effort = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- SNAPSHOT TX reads to pin vlsn, writes key 1.
        box.begin()
        s:get(1)
        s:replace{1, 'snapshot'}

        -- BEST_EFFORT TX writes key 1 and commits first.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin({txn_isolation = 'best-effort'})
            s:replace{1, 'best_effort'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX tries to commit and gets ww conflict.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'Transaction has been aborted')

        t.assert_equals(s:get(1), {1, 'best_effort'})
    end)
end

-- SNAPSHOT TX does a full scan, concurrent TX inserts into the gap,
-- SNAPSHOT TX inserts a different key and commits. No conflict.
g.test_gap_reads_no_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{10, 'b'}
        box.snapshot()

        -- SNAPSHOT TX scans the whole space (gap read).
        box.begin()
        s:select()

        -- Concurrent TX inserts into the gap.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{5, 'gap'}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX inserts a different key and commits.
        s:replace{20, 'c'}
        box.commit()

        t.assert_equals(s:get(5), {5, 'gap'})
        t.assert_equals(s:get(20), {20, 'c'})
    end)
end

-- SNAPSHOT TX inserting a duplicate key gets ER_TUPLE_FOUND
-- (unique check still works).
g.test_unique_constraint = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:insert{1, 'original'}
        box.snapshot()

        box.begin()
        local ok, err = pcall(s.insert, s, {1, 'duplicate'})
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'Duplicate key exists')
        box.rollback()

        t.assert_equals(s:get(1), {1, 'original'})
    end)
end

-- SNAPSHOT TX reads via secondary index, concurrent TX writes
-- the same primary key, SNAPSHOT TX commits successfully.
g.test_secondary_index_no_rw_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}})
        s:replace{1, 'a', 'data'}
        box.snapshot()

        -- SNAPSHOT TX reads via secondary index.
        box.begin()
        s.index.sk:get('a')

        -- Concurrent TX updates the same primary key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'a', 'updated'}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX writes a different key and commits.
        s:replace{2, 'b', 'new'}
        box.commit()

        t.assert_equals(s:get(1), {1, 'a', 'updated'})
        t.assert_equals(s:get(2), {2, 'b', 'new'})
    end)
end

-- ---------------------------------------------------------------
-- SNAPSHOT isolation on memtx should be equivalent to the default
-- memtx isolation level.
-- ---------------------------------------------------------------

-- Verify that SNAPSHOT on memtx doesn't break basic operations.
g.test_memtx_snapshot_basic = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_memtx', {engine = 'memtx'})
        s:create_index('pk')
        s:replace{1, 'a'}

        -- SNAPSHOT TX on memtx: read, write, commit.
        box.begin()
        t.assert_equals(s:get{1}, {1, 'a'})
        s:replace{2, 'b'}
        box.commit()

        t.assert_equals(s:get{2}, {2, 'b'})
    end)
end

-- Verify SNAPSHOT on memtx detects conflicts the same way as
-- the default isolation (best-effort). Currently SNAPSHOT falls
-- through to best-effort on memtx, so both abort on rw conflicts.
g.test_memtx_snapshot_conflict_matches_default = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test_memtx', {engine = 'memtx'})
        s:create_index('pk')
        s:replace{1, 'a'}

        -- Run the same scenario with both isolation levels.
        -- TX reads key 1, concurrent TX writes key 1, TX writes
        -- key 2 and tries to commit.
        local function run_scenario(isolation)
            s:replace{1, 'a'} -- reset
            box.begin({txn_isolation = isolation})
            s:get{1}

            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace{1, 'modified'}
                ch:put(true)
            end)
            ch:get()

            local ok = pcall(s.replace, s, {2, 'written'})
            if not ok then
                box.rollback()
            else
                ok = pcall(box.commit)
            end
            return ok
        end

        local be_ok = run_scenario('best-effort')
        local snap_ok = run_scenario('snapshot')

        -- Both should behave the same way on memtx:
        -- both abort on rw conflict (memtx has no special
        -- SNAPSHOT handling -- it falls through to best-effort).
        t.assert_equals(be_ok, false,
            'best-effort on memtx should abort on rw conflict')
        t.assert_equals(snap_ok, be_ok,
            'SNAPSHOT on memtx should match best-effort behavior')
    end)
end

-- ---------------------------------------------------------------
-- Mixed SNAPSHOT + BEST_EFFORT isolation.
--
-- A BEST_EFFORT (serializable) TX reads key A, then a SNAPSHOT
-- TX writes key A and commits. The BEST_EFFORT TX is correctly
-- sent to a read view and aborted (because it's not read-only
-- and not SNAPSHOT), preserving serializability.
-- ---------------------------------------------------------------
g.test_mixed_isolation_serializable_protected = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 1}
        s:replace{2, 1}
        box.snapshot()

        -- TX1: serializable, reads both keys.
        box.begin({txn_isolation = 'best-effort'})
        s:get{1}
        s:get{2}

        -- TX2: snapshot, writes A=2, commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 2}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 was sent to a read view (rw conflict on key A).
        -- Since TX1 is BEST_EFFORT and not read-only, it's aborted.
        local ok, err = pcall(s.replace, s, {2, 2})
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        box.rollback()

        -- Serializability preserved: only TX2's write is visible.
        t.assert_equals(s:get{1}, {1, 2})
        t.assert_equals(s:get{2}, {2, 1})
    end)
end

-- A BEST_EFFORT TX reads and writes key 1. A SNAPSHOT TX also
-- writes key 1 and commits first. The BEST_EFFORT TX should
-- be aborted because it read key 1 (rw conflict detected by
-- its own read tracking) and the SNAPSHOT TX wrote it.
g.test_mixed_isolation_ww_still_detected = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- TX1 (best-effort): reads key 1, then writes it.
        box.begin({txn_isolation = 'best-effort'})
        s:get{1}
        s:replace{1, 'be_tx'}

        -- TX2 (snapshot): writes key 1 and commits first.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 'snap_tx'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 (best-effort) is aborted: TX2's commit found
        -- TX1 in the read set for key 1 (rw conflict), and
        -- TX1 is not SNAPSHOT so it gets aborted.
        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'TX1 should abort (rw conflict)')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')

        t.assert_equals(s:get{1}, {1, 'snap_tx'})
    end)
end

-- ---------------------------------------------------------------
-- A snapshot TX detects WW conflicts with a best-effort
-- (read-committed) TX. The conflict is detected at prepare
-- time via the BPS tree's older entry.
g.test_mixed_isolation_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        local val = s:get(1)
        t.assert_equals(val, {1, 100})

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin({txn_isolation = 'best-effort'})
            s:replace{1, 200}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        local ok, _ = pcall(function()
            s:replace{1, val[2] + 10}
            box.commit()
        end)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'cross-isolation conflict detected')
        t.assert_equals(s:get(1), {1, 200})
    end)
end

-- ---------------------------------------------------------------
-- A snapshot TX that has been sent to a read view by a
-- concurrent write must still detect write-write conflicts
-- on subsequent writes.
-- ---------------------------------------------------------------
g.test_ww_conflict_after_read_view = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        s:replace{2, 'init'}
        box.snapshot()

        -- TX1 (snapshot): reads key 1 (registered in read set).
        box.begin()
        s:get{1}

        -- TX2: writes key 1 → TX1 sent to read view
        -- (rw conflict, but SNAPSHOT TX is not aborted).
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'tx2'}
            ch:put(true)
        end)
        ch:get()

        -- TX1 is now in a read view. Writes key 2.
        s:replace{2, 'tx1_b'}

        -- TX3: writes key 2 and commits.
        local ch2 = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'tx3'}
            ch2:put(true)
        end)
        ch2:get()

        -- TX1 should be aborted (ww conflict on key 2).
        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'TX1 should abort (ww conflict on key 2)')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- An aborted TX's write tracking entries remain registered
-- until the TX is destroyed. The write-write conflict check
-- must skip aborted TXs so they don't cause false conflicts.
--
-- Schedule:
-- 1. TX_A writes key 1
-- 2. TX_B writes key 1 and commits -- TX_A is aborted
-- 3. TX_C writes key 1 -- must not conflict with aborted TX_A
g.test_aborted_tx_no_false_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- TX_A: snapshot, writes key 1.
        box.begin()
        s:replace{1, 'tx_a'}

        -- TX_B: writes key 1 and commits first.
        -- TX_A is aborted (write-write conflict).
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 'tx_b'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX_A is aborted but not yet rolled back.
        -- Its write tracking entries are still registered.

        -- TX_C: writes key 1. Must not get a false conflict
        -- from the aborted TX_A.
        local ch2 = fiber.channel(1)
        local tx_c_ok
        fiber.create(function()
            box.begin()
            s:replace{1, 'tx_c'}
            tx_c_ok = pcall(box.commit)
            ch2:put(true)
        end)
        ch2:get()

        -- Roll back TX_A.
        box.rollback()

        t.assert(tx_c_ok, 'TX_C must not conflict with aborted TX_A')
        t.assert_equals(s:get(1), {1, 'tx_c'})
    end)
end

-- No conflict when the committed TX's write is visible to the
-- new TX. TX_A writes key 1 and commits. TX_B starts after
-- TX_A committed, so TX_B's read view includes TX_A's write.
-- TX_B writes key 1 and commits successfully -- no false
-- conflict from the already-visible write.
g.test_no_conflict_with_visible_committed_write = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- TX_A writes key 1 and commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 'tx_a'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX_B starts AFTER TX_A committed. Its read view
        -- includes TX_A's write, so writing the same key
        -- must not be a conflict.
        box.begin()
        t.assert_equals(s:get(1), {1, 'tx_a'})
        s:replace{1, 'tx_b'}
        box.commit()

        t.assert_equals(s:get(1), {1, 'tx_b'})
    end)
end

-- A pure-read snapshot TX must not leak read views. It goes
-- through the read-only path and is not added to the committed
-- list, so its read view must be properly released on commit.
g.test_read_only_snapshot_no_leak = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        box.snapshot()

        t.assert_equals(box.stat.vinyl().tx.read_views, 0)

        for i = 1, 10 do
            box.begin()
            s:replace{100, 'dummy'}
            s:get{1}

            -- Concurrent write so the read view can't be
            -- trivially shared with later TXs.
            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace{1, 'v' .. i}
                ch:put(true)
            end)
            ch:get()

            box.rollback()
        end

        t.assert_equals(box.stat.vinyl().tx.read_views, 0,
            'read views must not leak')
    end)
end

-- A read-only autocommit transaction -- an iterator opened outside an
-- explicit transaction -- runs as snapshot isolation unconditionally, not
-- the configured default. It takes a point-in-time read view and registers
-- nothing in the read set, so it does no read tracking, which would be pure
-- CPU cost for a read that can never conflict.
g.test_autocommit_readonly_is_snapshot = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do s:replace{i} end

        -- An autocommit iterator keeps its transaction alive between steps,
        -- so its read view shows up in box.stat.vinyl(). A snapshot TX takes
        -- a read view and, by construction, registers nothing in the read
        -- set; a read-confirmed TX does neither. Measure the delta this
        -- iterator adds, not the absolute count -- a read view leaked by an
        -- earlier test would otherwise let a read-confirmed regression pass.
        local before = box.stat.vinyl().tx.read_views

        local gen, param, state = s:pairs()
        local tuple
        state, tuple = gen(param, state)
        t.assert_not_equals(tuple, nil)
        local during = box.stat.vinyl().tx.read_views

        -- Drain the iterator before asserting; exhausting it closes the
        -- autocommit transaction and releases the read view, so a failed
        -- assertion below can't leak it into later tests.
        while state ~= nil do
            state = gen(param, state)
        end

        t.assert_ge(during - before, 1,
                    'read-only autocommit iterator runs as snapshot')
    end)
end

-- An iterator pins its read view at the first actual read, not at
-- open. A write committed between the open and the first step is
-- visible; a write committed after the first step is not.
g.test_autocommit_iterator_pins_view_at_first_read = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}

        local gen, param, state = s:pairs()
        s:replace{2} -- committed before the first read: visible
        local tuple
        state, tuple = gen(param, state)
        t.assert_equals(tuple, {1})
        s:replace{3} -- committed after the first read: invisible
        local rest = {}
        while true do
            state, tuple = gen(param, state)
            if state == nil then break end
            table.insert(rest, tuple)
        end
        t.assert_equals(rest, {{2}})
    end)
end

-- Same as the previous test, but for an iterator opened inside an
-- explicit transaction: the transaction's read view is assigned at
-- its first read, so a write committed after the iterator was opened
-- but before the transaction read anything is visible to it.
g.test_explicit_tx_iterator_pins_view_at_first_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}

        local function commit_async(tuple)
            local ch = fiber.channel(1)
            fiber.create(function()
                s:replace(tuple)
                ch:put(true)
            end)
            ch:get()
        end

        box.begin()
        local gen, param, state = s:pairs()
        commit_async({2}) -- before the first read: visible
        local tuple
        state, tuple = gen(param, state)
        t.assert_equals(tuple, {1})
        commit_async({3}) -- after the first read: invisible
        local rest = {}
        while true do
            state, tuple = gen(param, state)
            if state == nil then break end
            table.insert(rest, tuple)
        end
        box.commit()
        t.assert_equals(rest, {{2}})
    end)
end

-- ---------------------------------------------------------------
-- A snapshot TX whose read view is far behind the current LSN
-- (many concurrent commits happened since it started) must
-- still prepare and commit without corruption or assertions.
-- ---------------------------------------------------------------
g.test_prepare_from_stale_read_view = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 100 do
            s:replace{i, 'init'}
        end
        box.snapshot()

        -- TX1 (snapshot): reads and writes.
        box.begin()
        s:get{1}
        s:replace{101, 'tx1'}

        -- Advance the global LSN well past TX1's read view
        -- with many autocommit writes.
        for i = 1, 50 do
            s:replace{200 + i, 'concurrent_' .. i}
        end

        -- TX1 is now in a read view with a stale vlsn.
        s:replace{102, 'tx1_late'}
        box.commit()

        t.assert_equals(s:get{101}, {101, 'tx1'})
        t.assert_equals(s:get{102}, {102, 'tx1_late'})
        t.assert_equals(s:get{250}, {250, 'concurrent_50'})
    end)
end

-- Prepare from read view with concurrent dump/compaction.
g.test_prepare_from_read_view_with_dump = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 100 do
            s:replace{i, string.rep('x', 100)}
        end
        box.snapshot()

        box.begin()
        s:get{1}

        for i = 1, 100 do
            s:replace{i, string.rep('y', 100)}
        end
        box.snapshot()

        s:replace{101, 'from_read_view'}
        box.commit()

        t.assert_equals(s:get{101}, {101, 'from_read_view'})
        t.assert_equals(s:get{1}[2], string.rep('y', 100))
    end)
end

-- DML-time WW conflict resolved from the tuple cache. The
-- conflicting version is on disk (sealed mems freed, so the
-- sealed-mem scan misses) and warm in the cache, so the check
-- resolves the conflict from the cache, not a disk scan.
--   1. key 1 @ L1, dumped to disk.
--   2. TX1 (snapshot) reads key 1, pinning its read view at L1.
--   3. autocommit overwrites key 1 @ L2 > L1 and dumps, freeing
--      the sealed mems.
--   4. autocommit get{1} warms the tuple cache with key 1 @ L2.
--   5. TX1 writes key 1: sealed-mem miss, dump_lsn(L2) >= min_lsn,
--      cache hit -> first-committer-wins conflict at DML time.
g.test_ww_conflict_resolved_from_cache = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        -- (1) key 1 @ L1, flushed to disk.
        s:replace{1, 10}
        box.snapshot()

        -- (2) TX1 runs in its own fiber; the main fiber drives the
        --     concurrent writer, the dump and the cache warm-up
        --     (all autocommit, so the main fiber holds no
        --     transaction while it calls box.snapshot()).
        local to_tx1 = fiber.channel(0)
        local from_tx1 = fiber.channel(0)
        local result = {}
        fiber.create(function()
            box.begin()                       -- snapshot (from box.cfg)
            s:get{1}                          -- pin read view at L1
            from_tx1:put('read_done')
            to_tx1:get()                      -- wait for disk + cache setup
            -- (5) conflicting write; check must resolve via the cache.
            local ok, err = pcall(function() s:replace{1, 99} end)
            result.ok = ok
            result.err = tostring(err)
            pcall(box.rollback)
            from_tx1:put('write_done')
        end)
        from_tx1:get()                        -- TX1 has pinned its snapshot

        -- (3) concurrent committed overwrite @ L2, then dump so it
        --     lives on disk and the sealed mems are freed.
        s:replace{1, 20}
        box.snapshot()

        -- (4) warm the tuple cache for key 1 at the global read view.
        t.assert_equals(s:get{1}, {1, 20})

        -- Capture cache metrics around TX1's conflicting write so the
        -- test itself proves the conflict is resolved from the cache,
        -- not a disk scan. cache.lookup counts every cache probe;
        -- cache.get.rows counts only hits.
        local function pk_cache()
            local c = s.index.pk:stat().cache
            return c.lookup, c.get.rows
        end
        local lookups0, hits0 = pk_cache()

        to_tx1:put('go')
        from_tx1:get()

        local lookups1, hits1 = pk_cache()

        -- TX1's blind-snapshot write to a key changed after its
        -- snapshot is a first-committer-wins conflict at DML time.
        t.assert_not(result.ok)
        t.assert_str_contains(result.err, 'conflict')

        -- The WW check resolved the conflict from the tuple cache:
        -- one cache probe, and it hit (value served from the cache,
        -- no disk scan).
        t.assert_equals(lookups1 - lookups0, 1,
            'WW check must perform exactly one cache lookup')
        t.assert_equals(hits1 - hits0, 1,
            'the cache lookup must hit (value served from cache)')

        -- The committed value is the concurrent writer's, untouched.
        t.assert_equals(s:get{1}, {1, 20})
    end)
end

-- A snapshot TX that upserts a key with no prior version. The TX
-- reads first, so it is in a read view and its PK write goes through
-- the WW check; the upsert then runs vy_mem_insert_upsert, whose
-- previous-version lookup finds nothing (the key is new).
g.test_snapshot_upsert_new_key = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        box.begin({txn_isolation = 'snapshot'})
        s:get{1}                              -- pin the read view
        -- New key: vy_mem_insert_upsert finds no previous version.
        s:upsert({2, 100}, {{'+', 2, 1}})
        box.commit()

        t.assert_equals(s:get{2}, {2, 100})
        t.assert_equals(s:get{1}, {1, 10})
    end)
end

-- A snapshot TX that upserts a key a concurrent TX modified after
-- the snapshot's read view. The concurrent write stays in the active
-- mem, so vy_mem_insert_upsert returns it as the previous version and
-- the prepare-time PK check (vy_tx_check_primary) detects the
-- write-write conflict.
g.test_snapshot_upsert_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        box.begin({txn_isolation = 'snapshot'})
        s:get{1}                              -- pin read view at {1, 10}

        -- Concurrent committed write to key 1 (stays in the active mem).
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 20}
            ch:put(true)
        end)
        ch:get()

        -- Upsert key 1: the previous version found by
        -- vy_mem_insert_upsert is the concurrent write -> WW conflict.
        local ok = pcall(function() s:upsert({1, 0}, {{'+', 2, 1}}) end)
        if ok then ok = pcall(box.commit) else pcall(box.rollback) end
        t.assert_not(ok, 'snapshot upsert to a concurrently-modified '
                     .. 'key must conflict')

        t.assert_equals(s:get{1}, {1, 20})    -- the concurrent write won
    end)
end

-- ---------------------------------------------------------------
-- Unique secondary index: two SNAPSHOT TXs insert different
-- primary keys that collide on the unique secondary key.
-- ---------------------------------------------------------------
g.test_secondary_index_unique_ww_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = true})
        box.snapshot()

        box.begin()
        s:insert{1, 'collision'}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:insert{2, 'collision'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        local ok, err = pcall(box.commit)
        t.assert_not(ok,
            'TX1 should fail: ww conflict on unique SK')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        t.assert_equals(s:get{2}, {2, 'collision'})
    end)
end

-- A snapshot TX writes the same PK twice in one transaction.
-- The second write overwrites the first in the write set.
-- No false self-conflict should be detected.
g.test_double_write_same_key = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:replace{1, 200}
        s:replace{1, 300}
        box.commit()
        t.assert_equals(s:get(1), {1, 300})
    end)
end

-- A snapshot TX writes two different PKs that share a unique
-- SK value. The unique constraint check at DML time must
-- catch this (not at prepare time).
g.test_double_write_unique_sk_same_tx = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = true})
        box.snapshot()

        box.begin()
        s:insert{1, 'val'}
        local ok, err = pcall(s.insert, s, {2, 'val'})
        box.rollback()
        t.assert_not(ok,
            'unique SK violation within same TX')
        t.assert_str_contains(tostring(err), 'Duplicate key')
    end)
end

-- Unique secondary index write-write conflict where the primary
-- and secondary keys sort differently, exercising write tracking
-- across multiple indexes.
g.test_secondary_index_unique_ww_conflict_nsearch = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        -- Use an integer SK so its values sort differently from
        -- string keys, making it more likely that psearch lands
        -- on a PK entry rather than an SK entry.
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        box.snapshot()

        -- TX1 inserts a row. The write set will have entries
        -- for both PK (key=1) and SK (key=100).
        box.begin()
        s:insert{1, 100}

        -- TX2 inserts a different PK but same SK value.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:insert{2, 100}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 should detect ww conflict on SK value 100.
        local ok, err = pcall(box.commit)
        t.assert_not(ok,
            'TX1 should fail: ww conflict on unique SK')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- Unique secondary index write-write conflict when the snapshot
-- TX already has a read view at the time of the conflicting
-- insert. Write tracking for unique secondary keys must work
-- regardless of whether the TX has a read view.
g.test_secondary_index_unique_ww_conflict_in_read_view = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = true})
        s:replace{100, 'seed'}
        box.snapshot()

        -- TX1 reads key 100 so it has a read-set entry.
        box.begin()
        s:replace{101, 'dummy'}
        s:get{100}

        -- A concurrent write to key 100 sends TX1 to a read view.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{100, 'updated'}
            ch:put(true)
        end)
        ch:get()
        t.assert_equals(box.stat.vinyl().tx.read_views, 1,
            'TX1 should be in a read view')

        -- Now TX1 (in read view) inserts a row with unique SK
        -- value 'collision'.
        s:insert{1, 'collision'}

        -- TX2 inserts a different PK but same SK value.
        fiber.create(function()
            box.begin()
            s:insert{2, 'collision'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 should fail: ww conflict on unique SK.
        local ok, err = pcall(box.commit)
        t.assert_not(ok,
            'TX1 should fail: ww conflict on unique SK')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        t.assert_equals(s:get{2}, {2, 'collision'})
    end)
end

-- Unique secondary index: different key values → no conflict.
g.test_secondary_index_unique_no_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = true})
        box.snapshot()

        box.begin()
        s:insert{1, 'alpha'}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:insert{2, 'beta'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        box.commit()

        t.assert_equals(s:get{1}, {1, 'alpha'})
        t.assert_equals(s:get{2}, {2, 'beta'})
    end)
end

-- Non-unique secondary index: same key value → no conflict.
g.test_non_unique_secondary_no_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = false})
        box.snapshot()

        box.begin()
        s:insert{1, 'same_value'}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:insert{2, 'same_value'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        box.commit()

        t.assert_equals(s:get{1}, {1, 'same_value'})
        t.assert_equals(s:get{2}, {2, 'same_value'})
        t.assert_equals(#s.index.sk:select('same_value'), 2)
    end)
end

-- Write skew (G2-item) is allowed under snapshot isolation:
-- two TXs each read both keys, then write different keys.
g.test_write_skew_allowed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{2, 'b'}
        box.snapshot()

        box.begin()
        s:get{1}
        s:get{2}
        s:replace{1, 'x'}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:get{1}
            s:get{2}
            s:replace{2, 'y'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        box.commit()

        t.assert_equals(s:get{1}, {1, 'x'})
        t.assert_equals(s:get{2}, {2, 'y'})
    end)
end

-- ---------------------------------------------------------------
-- Gap and boundary tests.
-- Snapshot isolation permits phantoms (unlike SSI).
-- ---------------------------------------------------------------

-- Point read of a non-existing key, concurrent insert of that
-- exact key. SNAPSHOT TX should see "not found" consistently.
g.test_phantom_point_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        box.snapshot()

        box.begin()
        -- Key 5 doesn't exist.
        t.assert_equals(s:get{5}, nil)

        -- Concurrent TX inserts key 5.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{5, 'phantom'}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX still sees key 5 as non-existing.
        t.assert_equals(s:get{5}, nil)
        -- SNAPSHOT TX writes a different key and commits.
        s:replace{10, 'ok'}
        box.commit()

        -- After commit, key 5 is visible.
        t.assert_equals(s:get{5}, {5, 'phantom'})
        t.assert_equals(s:get{10}, {10, 'ok'})
    end)
end

-- Scan beyond max key (supremum). Concurrent TX inserts beyond
-- the current max. SNAPSHOT TX should not see the new row.
g.test_phantom_beyond_max_key = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{10, 'b'}
        box.snapshot()

        box.begin()
        local v1 = s:select()
        t.assert_equals(v1, {{1, 'a'}, {10, 'b'}})

        -- Concurrent TX inserts beyond max key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{100, 'beyond_max'}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX doesn't see the new row.
        local v2 = s:select()
        t.assert_equals(v2, {{1, 'a'}, {10, 'b'}})
        -- SNAPSHOT TX writes and commits.
        s:replace{50, 'mid'}
        box.commit()

        t.assert_equals(s:get{100}, {100, 'beyond_max'})
        t.assert_equals(s:get{50}, {50, 'mid'})
    end)
end

-- Scan before min key (infimum). Concurrent TX inserts before
-- the current min. SNAPSHOT TX should not see the new row.
g.test_phantom_before_min_key = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{10, 'a'}
        s:replace{20, 'b'}
        box.snapshot()

        box.begin()
        local v1 = s:select()
        t.assert_equals(v1, {{10, 'a'}, {20, 'b'}})

        -- Concurrent TX inserts before min key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'before_min'}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX doesn't see the new row.
        local v2 = s:select()
        t.assert_equals(v2, {{10, 'a'}, {20, 'b'}})
        box.commit()

        t.assert_equals(s:get{1}, {1, 'before_min'})
    end)
end

-- Both TXs try to insert the same non-existing key.
-- Write-write conflict on the new key.
g.test_phantom_ww_conflict_on_insert = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        box.snapshot()

        -- TX1 reads key 5 (doesn't exist), then inserts it.
        box.begin()
        t.assert_equals(s:get{5}, nil)
        s:replace{5, 'tx1'}

        -- TX2 also inserts key 5 and commits first.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{5, 'tx2'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 has ww conflict on key 5.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        t.assert_equals(s:get{5}, {5, 'tx2'})
    end)
end

-- ---------------------------------------------------------------
-- Additional scenario coverage.
-- ---------------------------------------------------------------

-- Concurrent TX rolls back -- SNAPSHOT TX should not be affected.
g.test_concurrent_rollback = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        box.begin()
        t.assert_equals(s:get{1}, {1, 'original'})

        -- Concurrent TX writes and rolls back.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 'will_rollback'}
            box.rollback()
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX sees original value, can write and commit.
        t.assert_equals(s:get{1}, {1, 'original'})
        s:replace{2, 'new'}
        box.commit()

        t.assert_equals(s:get{1}, {1, 'original'})
        t.assert_equals(s:get{2}, {2, 'new'})
    end)
end

-- DELETE visibility: SNAPSHOT TX reads a key, concurrent TX
-- deletes it, SNAPSHOT TX re-reads and still sees the value.
g.test_delete_visibility = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'alive'}
        s:replace{2, 'also_alive'}
        box.snapshot()

        box.begin()
        t.assert_equals(s:get{1}, {1, 'alive'})

        -- Concurrent TX deletes key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:delete{1}
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX still sees key 1 (from its snapshot).
        t.assert_equals(s:get{1}, {1, 'alive'})
        -- Range scan also sees it.
        t.assert_equals(#s:select(), 2)
        box.commit()

        -- After commit, key 1 is gone.
        t.assert_equals(s:get{1}, nil)
    end)
end

-- UPSERT under snapshot isolation.
g.test_blind_upsert_pk_only = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        box.begin()
        s:upsert({1, 10}, {{'+', 2, 5}})

        -- Concurrent TX also upserts the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:upsert({1, 10}, {{'+', 2, 100}})
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- Both upserts are blind (PK-only, no read view).
        -- With lazy vlsn, blind writes don't conflict.
        local ok = pcall(box.commit)
        t.assert(ok, 'blind upserts must not conflict')

        -- Both upserts applied.
        t.assert_equals(s:get{1}, {1, 115})
    end)
end

-- UPSERT that reads (non-blind) must still conflict.
-- Adding a unique SK forces vy_get during the upsert,
-- which assigns a read view. The concurrent upsert then
-- creates a WW conflict.
g.test_nonblind_upsert_unique_sk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 10}
        box.snapshot()

        box.begin()
        s:upsert({1, 10}, {{'+', 2, 5}})

        -- Concurrent TX upserts the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:upsert({1, 10}, {{'+', 2, 100}})
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- The upsert triggered vy_get (unique SK), which
        -- assigned a read view. WW conflict is detected.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')

        -- Only the concurrent TX's upsert applied.
        t.assert_equals(s:get{1}, {1, 110})
    end)
end

-- Two snapshot TXs created at the same LSN share one read
-- view. When one commits, the shared read view must not be
-- modified, or the other TX would see data committed after
-- its snapshot, breaking repeatable reads.
g.test_shared_read_view_not_mutated = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- TX1 and TX3 both write a dummy key to create a vinyl
        -- engine TX (reads alone use an autocommit path).
        -- They share a read view (created at the same LSN).
        box.begin()
        s:replace{300, 'tx1_dummy'}
        t.assert_equals(s:get{1}, {1, 'original'})

        -- TX3 uses a separate channel to avoid message mix-ups.
        local tx3_ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{200, 'tx3_dummy'}
            s:get{1}
            tx3_ch:put('started')
            tx3_ch:get()
            t.assert_equals(s:get{1}, {1, 'original'})
            box.commit()
            tx3_ch:put('done')
        end)
        tx3_ch:get()

        -- Concurrent write to key 1 commits after the shared
        -- read view was created.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'v1'}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(box.stat.vinyl().tx.read_views, 1,
            'TX1 and TX3 should share one read view')

        -- TX1 writes a different key so it's a RW TX.
        s:replace{100, 'tx1_write'}

        -- TX1 commits. The shared read view must not change,
        -- otherwise TX3 would see 'v1'.
        box.commit()

        tx3_ch:put('go')
        tx3_ch:get()
    end)
end

-- Committed snapshot TXs must release their read views
-- immediately on commit, not accumulate them. Verify that
-- the read view count stays bounded while a long-lived
-- active TX is present.
--
-- Schedule:
-- 1. TX_hold starts and stays active
-- 2. 20 short TXs commit, each at a new xm->lsn
-- 3. Their read views should be released on commit
g.test_committed_tx_releases_read_view = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        t.assert_equals(box.stat.vinyl().tx.read_views, 0)

        -- TX_hold: stays active with an open read view.
        -- Use unbuffered channel: s:replace may yield for
        -- disk WW conflict check, and put('go') must wait
        -- until the fiber reaches get(), not buffer 'go'
        -- where the main fiber's get() would pick it up.
        local hold_ch = fiber.channel(0)
        fiber.create(function()
            box.begin()
            s:replace{1, 'hold'}
            hold_ch:get()
            box.rollback()
            hold_ch:put('done')
        end)

        -- 20 short TXs. Each gets a new read view (xm->lsn
        -- advances between commits). Their read views should
        -- be released on commit.
        for i = 1, 20 do
            local ch = fiber.channel(1)
            fiber.create(function()
                box.begin()
                s:replace{100 + i, 'short'}
                box.commit()
                ch:put(true)
            end)
            ch:get()
        end

        -- Read views: TX_hold has 1. If committed TXs
        -- released their read views, the total is 1.
        -- If they didn't, it's 1 + up to 20.
        local rv = box.stat.vinyl().tx.read_views
        t.assert_le(rv, 2,
            ('committed TXs hold read views: %d'):format(rv))

        hold_ch:put('go')
        hold_ch:get()
    end)
end

-- A pure-read snapshot TX should not pin the committed list.
-- The read-only TX holds a read view (needed for repeatable
-- reads), but the conflict window ensures committed TXs are
-- pruned even if the read-only TX's read view is old. The
-- read-only TX still sees its original snapshot.
g.test_read_only_tx_does_not_pin_committed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- Read-only TX: reads key 1 but never writes.
        -- Use unbuffered channel: s:get may yield for disk
        -- I/O (vinyl_cache=0), and put('go') must wait until
        -- the fiber reaches get(), not buffer 'go' where the
        -- main fiber's get() would pick it up.
        local ro_ch = fiber.channel(0)
        fiber.create(function()
            box.begin()
            t.assert_equals(s:get(1), {1, 'init'})
            ro_ch:get()
            -- After many concurrent commits, the read-only
            -- TX must still see its original snapshot.
            t.assert_equals(s:get(1), {1, 'init'})
            box.rollback()
            ro_ch:put('done')
        end)

        -- 20 short write TXs commit while the read-only TX
        -- is active, including one that modifies key 1.
        for i = 1, 20 do
            local ch = fiber.channel(1)
            fiber.create(function()
                box.begin()
                if i == 10 then
                    s:replace{1, 'modified'}
                end
                s:replace{100 + i, 'write'}
                box.commit()
                ch:put(true)
            end)
            ch:get()
        end

        -- The read-only TX holds 1 read view (for repeatable
        -- reads). TX memory should stay bounded: committed
        -- TXs are pruned by the conflict window despite the
        -- read-only TX's old read view.
        local rv = box.stat.vinyl().tx.read_views
        t.assert_equals(rv, 1,
            ('read-only TX needs 1 read view, got %d'):format(rv))

        -- Release the read-only TX. It re-reads key 1 and
        -- must still see 'init' (repeatable read).
        ro_ch:put('go')
        ro_ch:get()

        local rv_after = box.stat.vinyl().tx.read_views
        t.assert_equals(rv_after, 0,
            ('read views after cleanup: %d'):format(rv_after))
    end)
end

-- Savepoint: partial rollback within a SNAPSHOT TX.
g.test_savepoint = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        box.snapshot()

        box.begin()
        s:replace{1, 'kept'}
        local sv = box.savepoint()
        s:replace{2, 'rolled_back'}
        box.rollback_to_savepoint(sv)
        s:replace{3, 'also_kept'}
        box.commit()

        t.assert_equals(s:get{1}, {1, 'kept'})
        t.assert_equals(s:get{2}, nil)
        t.assert_equals(s:get{3}, {3, 'also_kept'})
    end)
end

-- Dump during snapshot TX: force a memory dump while the TX
-- has an open read view. The TX must still see consistent data.
g.test_dump_during_snapshot = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i, 'v1'}
        end
        box.snapshot()

        -- SNAPSHOT TX reads some data to pin a read view.
        box.begin()
        t.assert_equals(s:get{1}, {1, 'v1'})

        -- Concurrent writes fill vy_mem (in a separate fiber).
        local ch = fiber.channel(1)
        fiber.create(function()
            for i = 1, 10 do
                s:replace{i, 'v2'}
            end
            box.snapshot()
            for i = 1, 10 do
                s:replace{i, 'v3'}
            end
            ch:put(true)
        end)
        ch:get()

        -- SNAPSHOT TX must still see 'v1' (from its read view).
        t.assert_equals(s:get{1}, {1, 'v1'})
        t.assert_equals(s:get{5}, {5, 'v1'})
        t.assert_equals(s:get{10}, {10, 'v1'})
        -- Full scan also consistent.
        local tuples = s:select()
        for _, tuple in ipairs(tuples) do
            t.assert_equals(tuple[2], 'v1')
        end
        box.commit()
    end)
end

-- Regression test: repeatable read must hold when a concurrent
-- writer prepares between two reads of the same key.
--
-- Writer A prepares key 1 (data in memory), then yields on WAL.
-- The reader starts and reads key 1. Writer B prepares on a
-- different key. The reader reads key 1 again. Both reads must
-- return the same value.
g.test_repeatable_read_prepared_then_committed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        box.cfg{vinyl_cache = 0}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'original'}
        box.snapshot()

        -- Writer A: autocommit replace. fiber.create runs it
        -- until the WAL yield. After the yield, key 1 = 'updated'
        -- is in memory but not yet committed.
        local ch_a = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'updated'}
            ch_a:put(true)
        end)

        -- Reader: snapshot TX created after writer A prepared.
        box.begin()
        s:replace{100, 'dummy'}

        -- First read.
        local v1 = s:get{1}

        -- Writer B: writes a different key.
        local ch_b = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'other'}
            ch_b:put(true)
        end)

        -- Let both writers complete.
        ch_a:get()
        ch_b:get()

        -- Second read: must return the same value as the first.
        local v2 = s:get{1}

        box.rollback()

        -- Both reads must return the same value.
        t.assert_equals(v1, v2,
            ('repeatable read violated: '  ..
             'first read %s, second read %s'):format(
                tostring(v1), tostring(v2)))
    end)
end

-- Lost update: two snapshot TXs read-modify-write the same key.
-- Writer A reads both keys first.  Then writer B reads both
-- keys, writes both, and commits.  Writer A writes based on
-- its (now stale) reads and commits -- overwriting B's update.
--
-- Under snapshot isolation, writer A must be aborted (write-write
-- conflict on key 1).  But because B's working set entries are
-- cleaned up on commit, A's prepare finds no conflict.
g.test_lost_update = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        s:replace{2, 100}
        box.snapshot()

        -- Writer A: read both keys.
        box.begin()
        local a1 = s:get(1)
        local a2 = s:get(2)
        t.assert_equals(a1, {1, 100})
        t.assert_equals(a2, {2, 100})

        -- Writer B: reads both keys, transfers 10 from key 1
        -- to key 2, commits.  Writer A has no writes yet, so
        -- no write-write conflict is detected.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            local b1 = s:get(1)
            local b2 = s:get(2)
            s:replace{1, b1[2] - 10}
            s:replace{2, b2[2] + 10}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- Writer A transfers 20 from key 1 to key 2, based on
        -- its stale reads. This conflicts with B's committed
        -- write to key 1. The conflict may be detected at DML
        -- time (small conflict window, slow path) or at commit
        -- time (large window, fast path).
        local ok, err = pcall(function()
            s:replace{1, a1[2] - 20}
            s:replace{2, a2[2] + 20}
            box.commit()
        end)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'writer A must be aborted: ' ..
            'lost update on key 1')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')

        -- The total must be preserved (B's transfer only).
        t.assert_equals(s:get(1)[2] + s:get(2)[2], 200,
            'total must be preserved')
    end)
end

-- ---------------------------------------------------------------
-- TOCTOU tests: a TX writes a key, then a concurrent TX writes
-- the same key and commits before the first TX prepares.
-- ---------------------------------------------------------------

-- TOCTOU: TX writes key 1, then another TX writes key 1
-- and commits. The conflict must be detected at prepare time.
g.test_toctou_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        s:replace{1, 110}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 200}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        local ok, err = pcall(box.commit)
        t.assert_not(ok,
            'TOCTOU conflict must be detected at prepare')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        t.assert_equals(s:get(1), {1, 200})
    end)
end

-- TOCTOU variant: TX writes key A, then key B. Between
-- the two writes, another TX writes key A and commits.
-- The conflict on key A must be detected.
g.test_toctou_between_writes = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        s:replace{2, 200}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        s:replace{1, 110}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 999}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        s:replace{2, 210}
        local ok = pcall(box.commit)
        t.assert_not(ok, 'conflict on key 1 must be detected')
    end)
end

-- ---------------------------------------------------------------
-- Long TX tests. A "long" TX is one that predates the current
-- vy_mem (vlsn < lsm->mem->min_lsn). It uses the slow path
-- (point lookup at DML time) for WW conflict detection.
-- vy_mem rotation is triggered by box.snapshot() which changes
-- the dump generation.
-- ---------------------------------------------------------------

-- A long TX writing a key that was modified by a concurrent
-- TX must detect the conflict at DML time.
g.test_long_tx_conflict_at_dml_time = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        -- TX_long starts. Read to pin the vlsn.
        box.begin()
        s:get(1)
        s:replace{1000, 'pin'}

        -- A concurrent TX writes key 1 and commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 200}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- Dump to trigger vy_mem rotation. After this,
        -- TX_long is "slow" (vlsn < lsm->mem->min_lsn).
        box.snapshot()

        -- TX_long writes key 1. The slow path detects
        -- the conflict at DML time.
        local ok, err = pcall(s.replace, s, {1, 110})
        box.rollback()

        t.assert_not(ok, 'long TX must detect conflict at DML time')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- A long TX writing a non-conflicting key should succeed.
g.test_long_tx_no_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:replace{1000, 'pin'}

        -- Concurrent TX writes a different key.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{2, 200}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- Dump to make TX_long "slow".
        box.snapshot()

        -- Key 1 was not touched by the concurrent TX.
        s:replace{1, 110}
        box.commit()

        t.assert_equals(s:get(1), {1, 110})
    end)
end

-- Unique secondary key conflict detection on the slow path.
-- A long TX inserts a row with a unique SK value that was
-- also inserted by a concurrent TX. The conflict must be
-- detected via the slow path point lookup on the SK.
g.test_slow_path_unique_sk_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'string'}, unique = true})
        box.snapshot()

        box.begin()
        s:replace{1000, 'pin'}

        -- Concurrent TX inserts with unique SK 'collision'.
        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:insert{2, 'collision'}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- Dump to make TX_long "slow".
        box.snapshot()

        -- TX_long inserts with the same unique SK value.
        local ok, _ = pcall(function()
            s:insert{3, 'collision'}
            box.commit()
        end)
        if not ok then pcall(box.rollback) end

        t.assert_not(ok, 'unique SK conflict must be detected')
    end)
end

-- After savepoint rollback, the tracking interval for the
-- rolled-back key must be removed. A concurrent write to
-- that key must NOT abort the TX.
g.test_savepoint_rollback_no_false_abort = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        s:replace{2, 'init'}
        box.snapshot()

        box.begin()
        local svp = box.savepoint()
        s:replace{1, 'first_attempt'}
        box.rollback_to_savepoint(svp)

        -- Concurrent TX writes key 1. Must NOT abort us
        -- because we rolled back our write to key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        -- Write a different key and commit.
        s:replace{2, 'unrelated'}
        box.commit()

        t.assert_equals(s:get(1), {1, 'concurrent'})
        t.assert_equals(s:get(2), {2, 'unrelated'})
    end)
end

-- Double write: TX writes key 1, savepoint, writes key 1
-- again (overwrites). Roll back second write. The first
-- write is restored and its tracking must survive.
g.test_savepoint_rollback_overwrite_keeps_tracking = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        s:replace{1, 'first_write'}
        local svp = box.savepoint()
        s:replace{1, 'second_write'}
        box.rollback_to_savepoint(svp)

        -- Concurrent TX writes key 1. Must detect conflict
        -- because our first write (restored) is still tracked.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        local ok, _ = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'conflict must be detected: ' ..
            'first write is still tracked')
    end)
end

-- Partial rollback: TX writes key 1 and key 2 in separate
-- statements. Roll back key 2. Concurrent TX writes key 1
-- must conflict. Concurrent TX writes key 2 must not.
g.test_savepoint_rollback_partial = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        s:replace{2, 'init'}
        box.snapshot()

        box.begin()
        s:replace{1, 'keep'}
        local svp = box.savepoint()
        s:replace{2, 'rollback_me'}
        box.rollback_to_savepoint(svp)

        -- Concurrent TX writes key 2 (rolled back). Must
        -- NOT cause a conflict.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        -- Our write to key 1 should still succeed.
        local ok = pcall(box.commit)
        t.assert(ok, 'commit must succeed: key 2 tracking ' ..
            'was removed on rollback')
        t.assert_equals(s:get(1), {1, 'keep'})
        t.assert_equals(s:get(2), {2, 'concurrent'})
    end)
end

-- Rollback + re-write: TX writes key 1, rolls back, writes
-- key 1 again. New tracking is created. Concurrent write to
-- key 1 must be detected.
g.test_savepoint_rollback_rewrite = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        local svp = box.savepoint()
        s:replace{1, 'first'}
        box.rollback_to_savepoint(svp)
        -- Re-write: creates a new tracking interval.
        s:replace{1, 'second'}

        -- Concurrent TX writes key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        local ok, _ = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'conflict must be detected: ' ..
            're-write created new tracking')
    end)
end

-- UPSERT with snapshot isolation. The UPSERT applies against
-- the snapshot's version of the data. A concurrent write to
-- the same key must be detected as a conflict.
g.test_upsert_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:get(1) -- pin vlsn
        s:replace{1000, 'pin'}

        -- Concurrent TX writes key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 200}
            ch:put(true)
        end)
        ch:get()

        -- UPSERT key 1 in the snapshot TX.
        local ok, _ = pcall(function()
            s:upsert({1, 0}, {{'+', 2, 10}})
            box.commit()
        end)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'UPSERT must conflict with ' ..
            'concurrent write')
    end)
end

-- DELETE + INSERT on the same key. A snapshot TX deletes a
-- key, then a concurrent TX inserts the same key. The
-- conflict must be detected.
g.test_blind_delete_pk_only = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        box.begin()
        s:delete{1}

        -- Concurrent TX writes key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        -- The DELETE is blind (PK-only space, no read view).
        -- With lazy vlsn, blind writes don't conflict.
        -- Both orderings are serializable.
        local ok = pcall(box.commit)
        t.assert(ok, 'blind DELETE must not conflict')
        -- The DELETE committed after the concurrent REPLACE,
        -- so the row is deleted.
        t.assert_equals(s:get(1), nil)
    end)
end

-- DELETE that reads (non-blind) must still conflict.
-- A read before the delete pins the read view, making
-- the concurrent write a WW conflict.
g.test_nonblind_delete_after_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        box.begin()
        -- Read pins the read view.
        t.assert_equals(s:get(1), {1, 'init'})
        s:delete{1}

        -- Concurrent TX writes key 1.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        -- The TX read before deleting, so it has a read view.
        -- The concurrent write creates a WW conflict.
        local ok, _ = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'DELETE after read must conflict')
        t.assert_equals(s:get(1), {1, 'concurrent'})
    end)
end

-- NULL-valued unique secondary key parts skip the conflict
-- check entirely. Two snapshot TXs inserting rows with NULL
-- in a unique SK part should both succeed.
-- This exercises the prepare-time (optimistic) path.
g.test_null_unique_sk_no_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned', is_nullable = true}},
                              unique = true})
        box.snapshot()

        box.begin()
        s:insert{1, box.NULL}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, box.NULL}
            ch:put(true)
        end)
        ch:get()

        -- Both inserts have NULL SK. No uniqueness conflict.
        local ok = pcall(box.commit)
        t.assert(ok, 'NULL SK values must not conflict')
        t.assert_equals(s:get(1), {1, box.NULL})
        t.assert_equals(s:get(2), {2, box.NULL})
    end)
end

-- Same as above but exercises the slow path (DML time).
-- The TX predates the current vy_mem due to box.snapshot().
g.test_null_unique_sk_no_conflict_slow_path = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned', is_nullable = true}},
                              unique = true})
        box.snapshot()

        box.begin()
        s:replace{1000, 1000}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, box.NULL}
            ch:put(true)
        end)
        ch:get()

        -- Dump to make TX "slow".
        box.snapshot()

        -- Insert with NULL SK via slow path. Must not conflict.
        s:insert{1, box.NULL}
        box.commit()

        t.assert_equals(s:get(1), {1, box.NULL})
        t.assert_equals(s:get(2), {2, box.NULL})
    end)
end

-- Same as above but exercises the seal-time path.
-- The concurrent NULL insert is in the vy_mem that gets sealed.
g.test_null_unique_sk_no_conflict_seal = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned', is_nullable = true}},
                              unique = true})
        box.snapshot()

        box.begin()
        s:insert{1, box.NULL}

        -- Concurrent TX inserts with NULL SK and commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, box.NULL}
            ch:put(true)
        end)
        ch:get()

        -- Dump triggers vy_mem rotation. The seal-time check
        -- must not abort us for NULL SK values.
        box.snapshot()

        local ok = pcall(box.commit)
        t.assert(ok, 'NULL SK values must not conflict on seal')
        t.assert_equals(s:get(1), {1, box.NULL})
        t.assert_equals(s:get(2), {2, box.NULL})
    end)
end

-- Unique SK conflict on disk between entries with different PKs.
-- TX_B inserts {2, 200} (SK key=200, PK=2) and the entry is
-- dumped to disk. TX_A then inserts {3, 200} (same SK key=200,
-- PK=3). The DML-time disk scan must detect the conflict even
-- though the PK values differ. This requires extracting the
-- SK-only key for the BPS tree search; searching with the full
-- {SK, PK} tuple would miss the entry because {200, 2} sorts
-- before {200, 3} in the tree.
g.test_unique_sk_conflict_on_disk_different_pk = function(cg)
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

        -- Dump TX_B's entry to disk.
        box.snapshot()

        -- TX_A: insert with SK=200, PK=3. TX_B's {200, 2} is
        -- on disk. The conflict check must find it despite the
        -- different PK.
        local ok = pcall(s.insert, s, {3, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end
        t.assert_not(ok, 'unique SK conflict on disk must be '
                     .. 'detected with different PKs')
    end)
end

-- Unique SK conflict on disk hidden behind an older entry with
-- a smaller PK. Insert/delete cycles create multiple entries
-- with the same SK value on disk. The disk scan with an
-- SK-only key finds the smallest-PK entry first. If its LSN
-- is below min_lsn, the scan must continue to find the
-- conflicting entry at a larger PK.
g.test_unique_sk_conflict_on_disk_multiple_pks = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})

        local function do_write(fn)
            local ch = fiber.channel(1)
            fiber.create(function()
                fn()
                ch:put(true)
            end)
            ch:get()
        end

        -- Dump the INSERT for pk=1 into Run 1. This makes
        -- the next dump non-last-level AND prevents the write
        -- iterator from optimizing away the DELETE for pk=1
        -- (since the INSERT it masks is in an older run).
        s:replace{1, 200}
        box.snapshot()

        -- Delete pk=1. The DELETE stays in mem.
        s:delete{1}

        -- TX_A starts. vlsn is after the delete.
        box.begin()
        s:get(1)

        -- Concurrent write: pk=8, SK=200. High LSN.
        do_write(function() s:replace{8, 200} end)

        -- Dump into Run 2. The write iterator keeps the
        -- DELETE for {200, 1} (masks INSERT in Run 1).
        -- Run 2 has: {200, 1, DELETE} and {200, 8, INSERT}.
        box.snapshot()

        -- TX_A inserts pk=12, SK=200. The disk scan with
        -- SK-only key "200" finds {200, 1, DELETE} first
        -- (smallest PK, from Run 2). Its LSN < min_lsn.
        -- The scan must continue to find {200, 8, INSERT}
        -- with LSN >= min_lsn.
        local ok = pcall(s.insert, s, {12, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end
        t.assert_not(ok, 'unique SK conflict on disk must be '
                     .. 'detected past older entries')
    end)
end
-- Unique SK conflict with defer_deletes: the cache entry for
-- the old SK value is invalidated by the chain-link cleanup
-- in vy_cache_invalidate, so the cache scan doesn't mask the
-- conflict on disk. This test verifies that the cache is
-- self-cleaning even with defer_deletes.
g.test_unique_sk_conflict_cache_with_defer_deletes = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            defer_deletes = true,
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        t.assert_gt(box.cfg.vinyl_cache, 0,
            'vinyl_cache must be enabled')

        -- Insert {1, 200} and dump. Then read SK=200 via
        -- read-committed to populate the SK cache.
        s:replace{1, 200}
        box.snapshot()
        local put_before = s.index.sk:stat().cache.put.rows
        box.begin({txn_isolation = 'read-committed'})
        s.index.sk:get(200)
        box.commit()
        local cache = s.index.sk:stat().cache
        t.assert_gt(cache.put.rows, put_before,
            'SK cache must be populated by the read')

        -- Change pk=1's SK from 200 to 300. With defer_deletes,
        -- no SK DELETE for {200, 1}. But the cache chain-link
        -- cleanup invalidates the {200, 1} cache entry anyway.
        local invalidate_before = cache.invalidate.rows
        s:replace{1, 300}
        cache = s.index.sk:stat().cache
        t.assert_gt(cache.invalidate.rows, invalidate_before,
            'SK cache entry must be invalidated by replace')

        -- TX_A starts after the replace.
        box.begin()
        s:get(1)

        -- Concurrent write: pk=8 takes SK=200.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{8, 200}
            ch:put(true)
        end)
        ch:get()

        -- Dump. {200, 8} goes to disk.
        box.snapshot()

        -- TX_A inserts pk=12 with SK=200. The conflict at
        -- {200, 8} must be detected (cache is clean).
        local ok = pcall(s.insert, s, {12, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end
        t.assert_not(ok, 'unique SK conflict must be detected '
                     .. 'with defer_deletes and cache')
    end)
end

-- Unique SK conflict hidden behind intermediate entries in mem.
-- Multiple PKs with the same SK value accumulate through
-- insert/delete/insert cycles. The intermediate entries
-- (pk=4, pk=8) are written BEFORE the snapshot TX starts, so
-- their LSNs are below min_lsn. The conflicting entry (pk=1)
-- is written AFTER the snapshot TX starts. The backward scan
-- in vy_mem_check_concurrent_write must iterate past the
-- intermediate entries to find the conflict at pk=1.
g.test_unique_sk_conflict_multiple_pks_in_mem = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        box.snapshot()

        local function do_write(fn)
            local ch = fiber.channel(1)
            fiber.create(function()
                fn()
                ch:put(true)
            end)
            ch:get()
        end

        -- Write intermediate entries BEFORE TX_A starts.
        -- These will have LSN < min_lsn.
        do_write(function() s:replace{4, 200} end)
        do_write(function() s:delete{4} end)
        do_write(function() s:replace{8, 200} end)
        do_write(function() s:delete{8} end)

        -- TX_A: snapshot TX, vlsn after the intermediate
        -- writes. min_lsn > LSNs of pk=4 and pk=8 entries.
        box.begin()
        s:get(1)

        -- Write the conflict AFTER TX_A starts. pk=1 < pk=4,
        -- so {200, 1} sorts before the intermediate entries
        -- in the BPS tree.
        do_write(function() s:replace{1, 200} end)

        -- TX_A inserts pk=12, sk=200. Backward scan from
        -- {200, 12} first hits pk=8 entries (LSN < min_lsn),
        -- then pk=4 entries (LSN < min_lsn), then pk=1
        -- (LSN >= min_lsn). Must not stop early.
        local ok = pcall(s.insert, s, {12, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end
        t.assert_not(ok, 'unique SK conflict must be detected '
                     .. 'past intermediate mem entries')
    end)
end

-- Same as test_unique_sk_conflict_multiple_pks_in_mem but the
-- conflicting entry has a HIGHER PK than the writing TX's entry.
-- The forward scan from lower_bound must iterate through entries
-- with the same SK but higher PKs, not just check one entry.
g.test_unique_sk_conflict_higher_pk_in_mem = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        box.snapshot()

        local function do_write(fn)
            local ch = fiber.channel(1)
            fiber.create(function()
                fn()
                ch:put(true)
            end)
            ch:get()
        end

        -- Write intermediate entries BEFORE TX_A starts.
        do_write(function() s:replace{4, 200} end)
        do_write(function() s:delete{4} end)

        -- TX_A: snapshot TX.
        box.begin()
        s:get(1)

        -- Write the conflict AFTER TX_A starts. pk=12 > pk=1,
        -- so {200, 12} sorts AFTER the writing TX's search
        -- position in the BPS tree.
        do_write(function() s:replace{12, 200} end)

        -- TX_A inserts pk=1, sk=200. Forward scan from
        -- {200, 1} must iterate past pk=4 entries (old LSN)
        -- to find pk=12 (LSN >= min_lsn).
        local ok = pcall(s.insert, s, {1, 200})
        if ok then
            ok = pcall(box.commit)
        else
            pcall(box.rollback)
        end
        t.assert_not(ok, 'unique SK conflict must be detected '
                     .. 'in forward scan past intermediate entries')
    end)
end

-- Unique SK conflict in the active mem at prepare time between
-- entries with different PKs. TX_A inserts {3, 200} (no conflict
-- at DML time). TX_B then inserts {2, 200} (same SK, different
-- PK) into the active mem. At prepare time,
-- vy_tx_check_unique_secondary scans the active mem and must
-- detect the conflict despite different PK values.
g.test_unique_sk_conflict_prepare_active_mem_different_pk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, inserts {3, 200} (no conflict yet).
        box.begin()
        s:get(1)
        s:insert{3, 200}

        -- TX_B: insert {2, 200} into the active mem.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, 200}
            ch:put(true)
        end)
        ch:get()

        -- TX_A prepares. vy_tx_check_unique_secondary scans
        -- the active mem and must find TX_B's {200, 2} despite
        -- TX_A's entry being {200, 3}.
        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'unique SK conflict in active mem '
                     .. 'must be detected with different PKs')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- Unique SK conflict in a sealed mem at prepare time between
-- entries with different PKs. TX_A inserts {3, 200}, TX_B
-- inserts {2, 200}. A DDL operation triggers mem rotation,
-- moving TX_B's entry to a sealed mem. At prepare time,
-- vy_tx_check_unique_secondary scans sealed mems and must
-- detect the conflict.
g.test_unique_sk_conflict_prepare_sealed_mem_different_pk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: snapshot TX, inserts {3, 200} (no conflict yet).
        box.begin()
        s:get(1)
        s:insert{3, 200}

        -- TX_B: insert {2, 200}.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:insert{2, 200}
            ch:put(true)
        end)
        ch:get()

        -- DDL on a different space bumps space_cache_version,
        -- causing mem rotation during TX_A's prepare. TX_B's
        -- entry moves to a sealed mem.
        fiber.create(function()
            box.schema.space.create('__bump'):drop()
            ch:put(true)
        end)
        ch:get()

        -- TX_A prepares. vy_tx_check_unique_secondary scans
        -- sealed mems and must find TX_B's {200, 2}.
        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'unique SK conflict in sealed mem at '
                     .. 'prepare must be detected with different PKs')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

g.test_ww_conflict_detected_after_rotation = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 100}
        box.snapshot()

        -- Touch a vinyl space to force vinyl TX creation
        -- and snapshot vlsn assignment before TX_B commits.
        box.begin()
        s:get(1)

        -- Concurrent autocommit write to the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 300}
            ch:put(true)
        end)
        ch:get()

        -- TX_A writes the same key.
        s:replace{1, 200}

        -- DDL on a different space bumps space_cache_version,
        -- causing vy_lsm_rotate_mem_if_required to rotate the
        -- vy_mem during TX_A's prepare.
        ch = fiber.channel(1)
        fiber.create(function()
            box.schema.space.create('__bump'):drop()
            ch:put(true)
        end)
        ch:get()

        local ok, err = pcall(box.commit)
        t.assert_not(ok, 'WW conflict must be detected')
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- Swapping unique SK values between two tuples in a single
-- snapshot TX must not trigger a false self-conflict. At
-- prepare time, the first replace writes a DELETE for token A
-- to the active mem. The second replace inserts token A.
-- vy_tx_check_unique_secondary must not treat the TX's own
-- DELETE as a concurrent write.
g.test_swap_unique_sk_no_self_conflict = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('token', {parts = {2, 'unsigned'},
                                 unique = true})
        s:replace{1, 100}
        s:replace{2, 200}
        box.snapshot()

        -- Reassign tokens via a free intermediary (300).
        -- Replace 1 frees token 100 and takes 300. Replace 2
        -- takes the now-free 100. No concurrent TX exists.
        -- At prepare time, replace 1's DELETE {100, 1} is in
        -- the active mem. Replace 2's INSERT {100, 2} must not
        -- treat it as a concurrent write (same TX).
        box.begin()
        s:replace{1, 300}
        s:replace{2, 100}
        box.commit()

        t.assert_equals(s:get(1), {1, 300})
        t.assert_equals(s:get(2), {2, 100})
    end)
end

-- Secondary index scan: a snapshot TX scans, then a
-- concurrent write modifies a key. The autocommit SK scan
-- must see the new data, not a stale cached version.
g.test_sk_scan_cache_with_concurrent_write = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        for i = 1, 5 do
            s:replace{i, i * 100}
        end
        box.snapshot()

        box.begin()
        local scan_result = s.index.sk:select()
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{3, 350}
            ch:put(true)
        end)
        ch:get()
        box.commit()

        t.assert_equals(scan_result, {
            {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500}
        })

        local result = s.index.sk:select()
        t.assert_equals(result, {
            {1, 100}, {2, 200}, {3, 350}, {4, 400}, {5, 500}
        }, 'autocommit SK scan must see the latest data')
    end)
end

-- WW conflict detection must handle multikey indexes correctly.
-- The multikey_idx is stored in entry.hint and must be passed to
-- tuple_key_contains_null and vy_stmt_extract_key instead of
-- MULTIKEY_NONE.
g.test_multikey_ww_conflict = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {
            unique = true,
            parts = {{'[2][*]', 'unsigned'}},
        })
        s:insert{1, {10, 20}}
        s:insert{2, {30}}
        box.snapshot()

        -- Concurrent autocommit write to a different multikey value.
        -- Must not conflict with the snapshot TX.
        box.begin()
        s:replace{3, {40, 50}}
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{4, {60}}
            ch:put(true)
        end)
        ch:get()
        -- The TX writes to disjoint multikey values, no conflict.
        box.commit()

        t.assert_equals(s:get(3), {3, {40, 50}})
        t.assert_equals(s:get(4), {4, {60}})
    end)
end

-- Multikey index with nullable parts and exclude_null must not
-- crash during WW conflict detection (tuple_key_contains_null
-- needs the correct multikey_idx).
g.test_multikey_nullable_ww_conflict = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {
            unique = true,
            parts = {{'[2][*]', 'unsigned', exclude_null = true}},
        })
        s:insert{1, {10, 20}}
        s:insert{2, {box.NULL}}
        s:insert{3}
        box.snapshot()

        -- Autocommit operations on the multikey index must work
        -- without assertions (the multikey_idx must be passed
        -- to tuple_key_contains_null, not MULTIKEY_NONE).
        s:replace{4, {30, box.NULL}}
        s:replace{5, {box.NULL, 40}}
        s:delete{2}

        t.assert_equals(s.index.sk:select(), {
            {1, {10, 20}},
            {1, {10, 20}},
            {4, {30, box.NULL}},
            {5, {box.NULL, 40}},
        })
    end)
end

-- A snapshot TX doing a point lookup on data that is only on
-- disk (no newer version in mem) should populate the tuple
-- cache, because the result is identical to the global result.
g.test_snapshot_tx_populates_cache = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'data'}
        box.snapshot()

        local before = s.index.pk:stat().cache.put.rows

        box.begin()
        t.assert_equals(s:get(1), {1, 'data'})
        box.commit()

        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 1,
            'snapshot TX should populate cache when no ' ..
            'entries were skipped')
    end)
end

-- A snapshot TX doing a point lookup when a newer version
-- exists in mem should NOT populate the cache, because the
-- result differs from the global result.
g.test_snapshot_tx_does_not_cache_when_skipped = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        box.snapshot()

        box.begin()
        -- Pin the read view.
        t.assert_equals(s:get(1), {1, 'old'})

        -- A concurrent TX writes a newer version to mem.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        -- This lookup will skip the newer version in mem.
        t.assert_equals(s:get(1), {1, 'old'})
        box.commit()

        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 0,
            'snapshot TX should not populate cache when ' ..
            'mem entries were skipped')
    end)
end

-- After a snapshot TX populates the cache, a subsequent
-- autocommit read should hit the cache instead of disk.
g.test_snapshot_tx_cache_benefits_next_read = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'data'}
        box.snapshot()

        -- Snapshot TX populates the cache.
        box.begin()
        t.assert_equals(s:get(1), {1, 'data'})
        box.commit()

        local before = s.index.pk:stat().cache.get.rows
        -- Autocommit read should hit the cache.
        t.assert_equals(s:get(1), {1, 'data'})
        local after = s.index.pk:stat().cache.get.rows
        t.assert_equals(after - before, 1,
            'autocommit read should hit cache populated ' ..
            'by snapshot TX')
    end)
end

-- A snapshot TX looking up a key that was inserted after the
-- snapshot should get NULL and must NOT cache it, because the
-- key exists at the global read view.
g.test_snapshot_tx_does_not_cache_null_when_skipped = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        box.snapshot()

        box.begin()
        -- Pin the read view.
        t.assert_equals(s:get(1), nil)

        -- A concurrent TX inserts the key after our snapshot.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        -- The snapshot TX doesn't see the key. The NULL result
        -- must not be cached because the key exists globally.
        t.assert_equals(s:get(1), nil)
        box.commit()

        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 0,
            'snapshot TX should not cache NULL when a ' ..
            'newer version was skipped')
    end)
end

-- A snapshot TX doing a point lookup when a newer version
-- exists on disk (not mem) should NOT populate the cache.
g.test_snapshot_tx_does_not_cache_when_disk_skipped = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        box.snapshot()

        box.begin()
        -- Pin the read view.
        t.assert_equals(s:get(1), {1, 'old'})

        -- A concurrent TX writes a new version, dump it to
        -- disk so the newer version is in a run, not mem.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()
        box.snapshot()

        local before = s.index.pk:stat().cache.put.rows
        -- The disk scan will skip the newer run.
        t.assert_equals(s:get(1), {1, 'old'})
        box.commit()

        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 0,
            'snapshot TX should not populate cache when ' ..
            'disk entries were skipped')
    end)
end

-- A snapshot point lookup pinned at the old version, while the cache
-- already holds the newer committed version, must return the old
-- version but must not overwrite the cache with it. The newer version
-- stays cached and a later latest read is still served it from the
-- cache.
--
-- vy_point_lookup_scan_cache() skips the newer cached version without
-- flagging staleness, but the same version is also in mem/disk, where
-- the scan does flag it, so the stale result is kept out of the cache.
g.test_snapshot_tx_point_does_not_overwrite_newer_cache = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        box.snapshot()

        local saw
        local to_tx = fiber.channel(0)
        local from_tx = fiber.channel(0)
        fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            t.assert_equals(s:get(1), {1, 'old'}) -- pin the read view
            from_tx:put('pinned')
            to_tx:get()
            saw = s:get(1)                        -- skips the cached 'new'
            box.commit()
            from_tx:put('done')
        end)
        from_tx:get()

        -- A concurrent commit creates the newer version and an
        -- autocommit read warms the cache with it.
        s:replace{1, 'new'}
        t.assert_equals(s:get(1), {1, 'new'})

        local put_before = s.index.pk:stat().cache.put.rows
        to_tx:put('go')
        from_tx:get()
        local put_after = s.index.pk:stat().cache.put.rows

        -- The snapshot saw the old version but did not cache it.
        t.assert_equals(saw, {1, 'old'})
        t.assert_equals(put_after - put_before, 0,
            'a stale snapshot point lookup must not overwrite the cache')

        -- The newer version is still cached: a latest read hits it.
        local get_before = s.index.pk:stat().cache.get.rows
        t.assert_equals(s:get(1), {1, 'new'})
        local get_after = s.index.pk:stat().cache.get.rows
        t.assert_gt(get_after - get_before, 0,
            'the newer version is still served from the cache')
    end)
end

-- A read through a unique secondary index resolves the full tuple
-- from the primary index. That resolved tuple must populate the
-- primary cache so a later read finds it without repeating the
-- lookup in the primary index. An autocommit read runs at the
-- committed read view, which is finite (not the open global one), so
-- caching here relies on the result being the latest, not on the
-- read view.
g.test_autocommit_secondary_read_populates_primary_cache = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        local before = s.index.pk:stat().cache.put.rows
        t.assert_equals(s.index.sk:get(100), {1, 100})
        local after = s.index.pk:stat().cache.put.rows

        t.assert_equals(after - before, 1,
            'an autocommit secondary read populates the ' ..
            'primary cache')
    end)
end

-- The same property for an explicit snapshot TX running at a finite
-- read view. The TX reads key 2 and a concurrent write to key 2 pins
-- it to a finite read view. Key 1 was never touched, so the secondary
-- read resolves the latest primary tuple -- a fresh result that must
-- be cached even though the read view is no longer the global one.
g.test_snapshot_tx_secondary_read_populates_primary_cache = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        s:replace{2, 200}
        box.snapshot()

        box.begin()
        -- Read key 2 so a concurrent change to it pins this TX to a
        -- finite read view.
        t.assert_equals(s:get(2), {2, 200})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 250}
            ch:put(true)
        end)
        ch:get()

        -- Key 1 is unchanged: the secondary read resolves the latest
        -- primary tuple and must cache it.
        local before = s.index.pk:stat().cache.put.rows
        t.assert_equals(s.index.sk:get(100), {1, 100})
        local after = s.index.pk:stat().cache.put.rows
        box.commit()

        t.assert_equals(after - before, 1,
            'a fresh secondary read in a snapshot TX populates ' ..
            'the primary cache')
    end)
end

-- A secondary read whose lookup in the primary index skipped a newer version
-- (invisible to the snapshot) is stale and must not populate the
-- primary cache, or a later latest read would get the old tuple.
-- The secondary key is unchanged, so the secondary entry still
-- resolves to the same primary key; only the tuple resolved from the
-- primary index is stale.
g.test_snapshot_tx_secondary_read_skips_stale_primary_cache = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100, 'v1'}
        box.snapshot()

        box.begin()
        -- Pin the read view at v1 (key 1 enters the read set).
        t.assert_equals(s.index.sk:get(100), {1, 100, 'v1'})

        -- A concurrent TX overwrites a non-indexed field.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 100, 'v2'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        -- The snapshot still sees v1, skipping v2: a stale result.
        t.assert_equals(s.index.sk:get(100), {1, 100, 'v1'})
        box.commit()

        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 0,
            'a stale secondary read must not populate the ' ..
            'primary cache')
    end)
end

-- A snapshot secondary read pinned at the old version, while the
-- primary cache already holds the newer committed tuple for the same
-- primary key, must return the old version but must not overwrite
-- the cache with it. The newer tuple stays cached and a later latest
-- secondary read is still served it from the primary cache.
g.test_snapshot_tx_secondary_read_does_not_overwrite_newer_cache =
function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100, 'v1'}
        box.snapshot()

        local saw
        local to_tx = fiber.channel(0)
        local from_tx = fiber.channel(0)
        fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            -- Pin the read view at v1.
            t.assert_equals(s.index.sk:get(100), {1, 100, 'v1'})
            from_tx:put('pinned')
            to_tx:get()
            saw = s.index.sk:get(100) -- resolves primary key 1 at v1
            box.commit()
            from_tx:put('done')
        end)
        from_tx:get()

        -- A concurrent commit creates the newer version and an
        -- autocommit secondary read warms the primary cache with it.
        s:replace{1, 100, 'v2'}
        t.assert_equals(s.index.sk:get(100), {1, 100, 'v2'})

        local put_before = s.index.pk:stat().cache.put.rows
        to_tx:put('go')
        from_tx:get()
        local put_after = s.index.pk:stat().cache.put.rows

        -- The snapshot saw the old version but did not cache it.
        t.assert_equals(saw, {1, 100, 'v1'})
        t.assert_equals(put_after - put_before, 0,
            'a stale secondary resolution must not overwrite the ' ..
            'primary cache')

        -- The newer tuple is still cached: the latest secondary
        -- read serves it, and a primary point read is served from
        -- the cache.
        t.assert_equals(s.index.sk:get(100), {1, 100, 'v2'})
        local get_before = s.index.pk:stat().cache.get.rows
        t.assert_equals(s:get(1), {1, 100, 'v2'})
        local get_after = s.index.pk:stat().cache.get.rows
        t.assert_gt(get_after - get_before, 0,
            'the newer tuple is still served from the primary cache')
    end)
end

-- An autocommit range scan runs at the committed read view, which is
-- finite. Its results are the latest, so the scan must populate the
-- cache and let a later scan hit it instead of re-reading disk.
g.test_autocommit_scan_caches_values = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{2}
        s:replace{3}
        box.snapshot()

        -- First scan warms the cache from disk.
        t.assert_equals(s:select(), {{1}, {2}, {3}})

        local before = s.index.pk:stat().cache.get.rows
        -- Second scan must be served from the cache.
        t.assert_equals(s:select(), {{1}, {2}, {3}})
        local after = s.index.pk:stat().cache.get.rows

        t.assert_gt(after - before, 0,
            'an autocommit scan populates the cache so a later ' ..
            'scan hits it')
    end)
end

-- A scan skipping a deferred DELETE -- a compaction-only purge marker
-- in a secondary index -- must still populate the cache. The marker
-- hides nothing from any reader: the row's disappearance is
-- discovered through the lookup in the primary index, and a gap
-- cached across the marker is protected by the chain link LSN.
g.test_scan_caches_across_deferred_delete = function(cg)
    cg.server:exec(function()
        local defer_deletes = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false, parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- The delete writes only the primary index. The deferred
        -- DELETE for sk 10 is produced when primary compaction merges
        -- the tombstone with the replace it shadows; it is committed
        -- by a transaction of its own into the secondary index's
        -- active in-memory level.
        s:delete{1}
        if s.index.sk:stat().memory.rows > 0 then
            t.skip('the DELETE reaches the secondary index '
                   .. 'immediately: no deferred DELETE to scan across')
        end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({}, function()
            t.assert_gt(s.index.sk:stat().memory.rows, 0,
                        'the deferred DELETE must sit in the '
                        .. 'secondary index memory')
        end)

        local before = s.index.sk:stat().cache.put.rows
        -- sk 10 resolves to a deleted row and is filtered out; the
        -- scan must still cache what it returns.
        t.assert_equals(s.index.sk:select({}, {fullscan = true}),
                        {{2, 20}})
        local after = s.index.sk:stat().cache.put.rows
        t.assert_gt(after - before, 0,
            'a scan that skipped a deferred DELETE still populates '
            .. 'the cache')
        box.cfg{vinyl_defer_deletes = defer_deletes}
    end)
end

g.after_test('test_scan_caches_across_deferred_delete', function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_defer_deletes = false}
    end)
end)

-- Same as the previous test, but the deferred DELETE is dumped to a
-- run before the scan: skipping it from a disk source must not
-- suppress caching either.
g.test_scan_caches_across_deferred_delete_on_run = function(cg)
    cg.server:exec(function()
        local defer_deletes = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        -- A high run count threshold keeps automatic compaction away:
        -- it would annihilate the deferred DELETE with the replace it
        -- purges before the scan gets to see it.
        s:create_index('sk', {unique = false, parts = {{2, 'unsigned'}},
                              run_count_per_level = 10})
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        s:delete{1}
        if s.index.sk:stat().memory.rows > 0 then
            t.skip('the DELETE reaches the secondary index '
                   .. 'immediately: no deferred DELETE to scan across')
        end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({}, function()
            t.assert_gt(s.index.sk:stat().memory.rows, 0,
                        'the deferred DELETE must sit in the '
                        .. 'secondary index memory')
        end)

        -- Dump the deferred DELETE to a run of its own.
        box.snapshot()
        t.assert_equals(s.index.sk:stat().memory.rows, 0)
        t.assert_equals(s.index.sk:stat().run_count, 2)

        local before = s.index.sk:stat().cache.put.rows
        t.assert_equals(s.index.sk:select({}, {fullscan = true}),
                        {{2, 20}})
        local after = s.index.sk:stat().cache.put.rows
        t.assert_gt(after - before, 0,
            'a scan that skipped a deferred DELETE on a run still '
            .. 'populates the cache')

        -- The reverse scan skips it from the other end. Reset the
        -- cache first: the forward scan above cached the row and its
        -- chain, and a re-read served by the cache admits nothing.
        local cache_size = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}
        box.cfg{vinyl_cache = cache_size}

        before = s.index.sk:stat().cache.put.rows
        t.assert_equals(s.index.sk:select({}, {iterator = 'le',
                                               fullscan = true}),
                        {{2, 20}})
        after = s.index.sk:stat().cache.put.rows
        t.assert_gt(after - before, 0,
            'a reverse scan that skipped a deferred DELETE on a run '
            .. 'still populates the cache')
        box.cfg{vinyl_defer_deletes = defer_deletes}
    end)
end

g.after_test('test_scan_caches_across_deferred_delete_on_run',
             function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_defer_deletes = false}
    end)
end)

-- A unique secondary read runs through the range iterator (the unique
-- key is not a full cmp_def key). At the committed read view it must
-- populate the secondary index's own cache, so a repeat read hits it.
g.test_autocommit_secondary_read_populates_secondary_cache = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 100}
        box.snapshot()

        -- First read warms the secondary cache.
        t.assert_equals(s.index.sk:get(100), {1, 100})

        local before = s.index.sk:stat().cache.get.rows
        t.assert_equals(s.index.sk:get(100), {1, 100})
        local after = s.index.sk:stat().cache.get.rows

        t.assert_gt(after - before, 0,
            'an autocommit secondary read populates the secondary ' ..
            'cache so a later read hits it')
    end)
end

-- A snapshot scan that skips a newer version of a key must not cache
-- the stale value, or a later latest read would be served the old
-- tuple from the cache.
g.test_snapshot_tx_scan_does_not_cache_stale_value = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        s:replace{2, 'keep'}
        box.snapshot()

        box.begin()
        -- A gap scan pins the read view.
        t.assert_equals(s:select(), {{1, 'old'}, {2, 'keep'}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        -- The snapshot still sees the old value, skipping the newer one.
        t.assert_equals(s:select(), {{1, 'old'}, {2, 'keep'}})
        box.commit()

        -- A later latest read must see the new value, not a cached stale one.
        t.assert_equals(s:select(), {{1, 'new'}, {2, 'keep'}})
    end)
end

-- Two non-adjacent keys are each shadowed by a newer version that one
-- source advance skips in a single sweep. The scan must not cache
-- either stale value -- the second must not slip through after the
-- first. (A per-step staleness reset would miss the second.)
g.test_snapshot_tx_scan_multi_shadow_not_cached = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        s:replace{2, 'keep'}
        s:replace{3, 'old'}
        box.snapshot()

        box.begin()
        t.assert_equals(s:select(), {{1, 'old'}, {2, 'keep'}, {3, 'old'}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            s:replace{3, 'new'}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select(), {{1, 'old'}, {2, 'keep'}, {3, 'old'}})
        box.commit()

        -- Both shadowed keys must read the new version afterwards.
        t.assert_equals(s:select(), {{1, 'new'}, {2, 'keep'}, {3, 'new'}})
    end)
end

-- A reverse scan whose result is shadowed by a newer version in the
-- same source must not cache the stale value. The LE/LT search starts
-- from the oldest version, so the staleness is detected only by the
-- reverse reposition in find_lsn, not the forward skip loop.
g.test_snapshot_tx_reverse_scan_does_not_cache_stale = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        s:replace{2, 'keep'}
        -- No snapshot: {1, 'old'} stays in the active mem, so the old
        -- and new versions end up in the same source.
        box.begin()
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'old'}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'old'}})
        box.commit()

        -- A later latest read must see the new value, not a cached
        -- stale one (checked through both index orders).
        t.assert_equals(s:get(1), {1, 'new'})
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'new'}})
    end)
end

-- Same as the previous test, but the shadowed key has two visible
-- versions, not one. The reverse reposition then jumps straight to
-- the newest visible version instead of stepping over the invisible
-- one, and must still detect that the result is shadowed.
g.test_snapshot_tx_reverse_scan_two_versions_does_not_cache_stale =
function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'v1'}
        s:replace{1, 'v2'}
        s:replace{2, 'keep'}
        box.begin()
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'v2'}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'v3'}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'v2'}})
        box.commit()

        -- A later latest read must see the newest version, not a
        -- cached stale one.
        t.assert_equals(s:get(1), {1, 'v3'})
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{2, 'keep'}, {1, 'v3'}})
    end)
end

-- A reverse snapshot scan meets a key whose newer version was
-- committed after the view was pinned. The shadow provably concerns
-- that key alone, so while the scan stands on it, its other
-- results are still cached; only the overwritten key is withheld.
g.test_snapshot_tx_reverse_scan_caches_fresh_before_updated_key =
function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{20, 'disk'}
        box.snapshot()
        s:replace{10, 'old'}

        box.begin()
        -- Pin the read view with a miss: a point read of an existing
        -- key would cache it right away and hide the scan's own
        -- admission from the counter below.
        t.assert_equals(s:get(999), nil)

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{10, 'new'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{20, 'disk'}, {10, 'old'}})
        local after = s.index.pk:stat().cache.put.rows
        box.commit()

        -- Key 20 is fresh and must be cached; only key 10 is
        -- withheld.
        t.assert_gt(after - before, 0,
            'a fresh key next to a concurrently updated one is cached')

        -- The updated key was not cached stale: a latest read sees it.
        t.assert_equals(s:get(10), {10, 'new'})
        t.assert_equals(s:select({}, {iterator = 'le'}),
                        {{20, 'disk'}, {10, 'new'}})
    end)
end

-- A full-key EQ select travels the range read iterator, not the point
-- lookup. A source holding only an invisible newer version of the key
-- skips it entirely and never joins the merge front, so its testimony
-- must still reach the scan: the stale result must not be cached.
g.test_snapshot_tx_eq_select_does_not_cache_stale = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        box.snapshot()
        -- A committed row in the same in-memory level as the
        -- concurrent write, so the level is scanned rather than
        -- pruned as entirely invisible.
        s:replace{5, 'mem'}

        box.begin()
        t.assert_equals(s:select(2), {}) -- pin the read view

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select(1), {{1, 'old'}})
        box.commit()

        -- A later latest read must see the newest version, not a
        -- cached stale one.
        t.assert_equals(s:get(1), {1, 'new'})
        t.assert_equals(s:select(1), {{1, 'new'}})
    end)
end

-- A snapshot scan that skips a key concurrently inserted into a gap
-- must not cache the gap as empty, or a later latest read would miss
-- the inserted key.
g.test_snapshot_tx_scan_does_not_cache_stale_gap = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{3}
        box.snapshot()

        box.begin()
        t.assert_equals(s:select(), {{1}, {3}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select(), {{1}, {3}})
        box.commit()

        -- A later latest read must see the inserted key: the stale
        -- gap must not have been cached as empty.
        t.assert_equals(s:select(), {{1}, {2}, {3}})
    end)
end

-- Same as the stale-gap case but for the gap past the last key (EOF):
-- a key concurrently inserted beyond the snapshot's max must appear
-- in a later latest scan.
g.test_snapshot_tx_scan_does_not_cache_stale_eof = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{2}
        box.snapshot()

        box.begin()
        t.assert_equals(s:select(), {{1}, {2}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{3}
            ch:put(true)
        end)
        ch:get()

        t.assert_equals(s:select(), {{1}, {2}})
        box.commit()

        -- The key inserted beyond the snapshot's max must be visible.
        t.assert_equals(s:select(), {{1}, {2}, {3}})
    end)
end

-- A multikey index entry invisible to a snapshot read view sits on a run
-- (dumped after the view was pinned). The snapshot scan must skip it
-- without caching the gap across it; a cached "empty" gap would hide
-- the key from later readers -- a consistency violation.
g.test_multikey_snapshot_scan_skips_invisible_run_key = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('mk', {unique = false,
                              parts = {{'[5][*]', 'unsigned'}}})

        s:replace{1, 1, 1, 1, {10}}
        s:replace{2, 1, 1, 1, {30}}
        box.snapshot()

        box.begin()
        s.index.mk:count() -- pin the snapshot read view

        -- Concurrently insert mk 20 (between 10 and 30) and dump it to a run,
        -- so it is invisible to the pinned view yet lives on disk.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{3, 1, 1, 1, {20}}
            box.snapshot()
            ch:put(true)
        end)
        ch:get()

        -- Scan at the snapshot view: mk 20 is invisible and on a run. The
        -- scan must not cache the gap 10..30 as empty.
        t.assert_equals(s.index.mk:select({}, {fullscan = true}),
                        {{1, 1, 1, 1, {10}}, {2, 1, 1, 1, {30}}})
        box.commit()

        -- A latest reader must still see mk 20; if the gap was cached, it is
        -- served from the cache and mk 20 is lost.
        t.assert_equals(s.index.mk:select({}, {fullscan = true}),
                        {{1, 1, 1, 1, {10}}, {3, 1, 1, 1, {20}},
                         {2, 1, 1, 1, {30}}})
    end)
end

-- The following two tests characterize a known imperfection of the
-- is_stale mechanism: it is a per-source bit, not per-key, so a source
-- that skipped a newer invisible version conservatively suppresses
-- caching of fresh keys it did not actually shadow. The results are
-- still correct (we never cache a stale value); we just cache less
-- than we safely could. They are marked xfail -- anyone is welcome to
-- improve the solution (e.g. track per-key staleness), which will turn
-- them green; remove the xfail marker then.

-- A source yields a fresh value immediately after skipping an invisible
-- newer version of a different key. The fresh value could be cached but
-- is not, because the source is still flagged stale from the skip.
-- xfail is asserted on the client, since the cache delta is measured
-- inside the server.
g.test_snapshot_tx_scan_caches_fresh_after_same_source_skip = function(cg)
    local delta = cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{2, 'fresh'}
        -- No snapshot: {2, 'fresh'} stays in the active mem, so the
        -- skipped key and the fresh one share a source.
        box.begin()
        t.assert_equals(s:select(), {{2, 'fresh'}}) -- pin the read view

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'new'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        -- The scan skips {1, 'new'} and yields the fresh {2, 'fresh'}.
        t.assert_equals(s:select(), {{2, 'fresh'}})
        local after = s.index.pk:stat().cache.put.rows
        box.commit()
        return after - before
    end)

    t.xfail('is_stale is per-source, not per-key: the fresh value a '
            .. 'source yields right after skipping an invisible key is '
            .. 'not cached. Improving this (per-key staleness) is welcome.')
    t.assert_gt(delta, 0,
        'a fresh value after a same-source skip should be cached')
end

-- A scan returns fresh keys from one source while another source is
-- positioned far ahead on an invisible key it skipped. The fresh keys could
-- be cached but are not, because that source stays flagged stale for
-- the whole scan even though it holds none of those keys. The read view
-- is pinned via a key outside the scanned range so the measured scan is
-- the first to cache 1, 2, 3 (no pre-cached intervals, no re-put).
g.test_snapshot_tx_scan_caches_fresh_before_invisible = function(cg)
    local delta = cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{2}
        s:replace{3}
        s:replace{50, 'x'}
        box.snapshot()

        box.begin()
        t.assert_equals(s:get(50), {50, 'x'}) -- pin outside the scan range

        local ch = fiber.channel(1)
        fiber.create(function()
            -- Overwriting key 50 pins the TX and leaves a far invisible
            -- version in mem, ahead of the scanned range.
            s:replace{50, 'y'}
            ch:put(true)
        end)
        ch:get()

        local before = s.index.pk:stat().cache.put.rows
        -- Scan only 1, 2, 3; the mem source, positioned on the far invisible
        -- {50, 'y'} it skipped, keeps the whole scan flagged stale.
        t.assert_equals(s:select({1}, {iterator = 'ge', limit = 3}),
                        {{1}, {2}, {3}})
        local after = s.index.pk:stat().cache.put.rows
        box.commit()
        return after - before
    end)

    t.xfail('is_stale over-approximates: a far-ahead invisible write '
            .. 'suppresses caching of earlier fresh keys the source '
            .. 'does not hold. Improving this (per-key staleness) is welcome.')
    t.assert_gt(delta, 0,
        'fresh keys before an invisible write should be cached')
end

-- A blind write TX followed by a range scan. The scan goes
-- through vinyl_index_create_iterator which must trigger lazy
-- read view assignment. The concurrent commit between the
-- blind write and the scan must be visible (vlsn pinned at
-- scan time, after the commit).
g.test_blind_replace_pk_only_then_scan = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{2, 'b'}
        box.snapshot()

        box.begin()
        -- Blind write (PK-only, no read).
        s:replace{3, 'c'}

        -- Concurrent TX commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{4, 'd'}
            ch:put(true)
        end)
        ch:get()

        -- Range scan pins the vlsn AFTER the concurrent commit.
        -- It must see keys 1, 2, 4 (committed) + 3 (own write).
        -- Key 4 is visible because the vlsn includes it.
        local result = s:select()
        t.assert_equals(result, {
            {1, 'a'}, {2, 'b'}, {3, 'c'}, {4, 'd'}
        })
        box.commit()
    end)
end

-- A blind write TX must not be affected by a dump completing
-- during its lifetime. The dump-completion WW check walks
-- snapshot_active, but blind TXs are not on the list.
g.test_blind_replace_pk_only_during_dump = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}

        -- Concurrent TX writes the same key and commits.
        -- This puts an entry in the active mem.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        -- Blind write TX.
        box.begin()
        s:replace{1, 'blind'}

        -- Trigger dump. The dump-completion check must not
        -- crash or abort the blind TX (it's not on
        -- snapshot_active).
        box.snapshot()

        -- The blind TX commits successfully.
        local ok = pcall(box.commit)
        t.assert(ok, 'blind write must survive dump')
        t.assert_equals(s:get(1), {1, 'blind'})
    end)
end

-- Prepare-time WW conflict must not cascade-abort unrelated
-- snapshot TXs. The cascading abort in rollback_after_prepare
-- must only fire on WAL failure, not on prepare-time conflict.
g.test_prepare_conflict_no_cascade = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        s:replace{1, 100}
        box.snapshot()

        -- TX_A: read-then-write, will conflict at prepare.
        box.begin()
        s:get(1)
        s:replace{1, 200}

        -- TX_B: concurrent commit to the same key.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 300}
            ch:put(true)
        end)
        ch:get()

        -- TX_C: unrelated snapshot TX with a prepared read
        -- view (reads a different key).
        local tx_c_ch = fiber.channel(0)
        fiber.create(function()
            box.begin()
            local val = s:get(2)
            tx_c_ch:put(val)
            tx_c_ch:get()
            local ok = pcall(box.commit)
            if not ok then pcall(box.rollback) end
            tx_c_ch:put(ok)
        end)
        tx_c_ch:get()

        -- TX_A commits and fails (WW conflict). This must
        -- NOT cascade-abort TX_C.
        local ok = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'TX_A must conflict')

        -- TX_C must still be alive and able to commit.
        tx_c_ch:put('go')
        local tx_c_ok = tx_c_ch:get()
        t.assert(tx_c_ok, 'TX_C must not be cascade-aborted '
                           .. 'by TX_A prepare failure')
    end)
end

-- ---------------------------------------------------------------
-- Blind write coverage: defer_deletes cases.
-- With non-unique SK + defer_deletes, REPLACE and DELETE skip
-- the old tuple lookup, making them blind.
-- ---------------------------------------------------------------

g.test_blind_replace_nonunique_sk_defer_deletes = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            defer_deletes = true,
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:replace{1, 200}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 300}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        t.assert(ok, 'REPLACE with non-unique SK + defer_deletes '
                     .. 'is blind, must not conflict')
    end)
end

g.test_blind_delete_nonunique_sk_defer_deletes = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {
            engine = 'vinyl',
            defer_deletes = true,
        })
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:delete{1}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 300}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        t.assert(ok, 'DELETE with non-unique SK + defer_deletes '
                     .. 'is blind, must not conflict')
    end)
end

-- ---------------------------------------------------------------
-- Non-blind boundary cases: operations that trigger vy_get
-- and thus assign a read view, causing conflicts.
-- ---------------------------------------------------------------

-- REPLACE with non-unique SK but NO defer_deletes: the old
-- tuple lookup triggers vy_get, assigning a read view.
g.test_nonblind_replace_nonunique_sk_no_defer = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:replace{1, 200}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 300}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'REPLACE with non-unique SK (no defer) '
                         .. 'triggers vy_get, must conflict')
    end)
end

-- INSERT always checks PK uniqueness via vy_get.
g.test_nonblind_insert = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        box.begin()
        s:insert{2, 'new'}

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{2, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'INSERT checks PK uniqueness via vy_get, '
                         .. 'must conflict')
    end)
end

-- UPSERT with any SK triggers vy_get for old tuple lookup.
g.test_nonblind_upsert_with_sk = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = false})
        s:replace{1, 100}
        box.snapshot()

        box.begin()
        s:upsert({1, 100}, {{'+', 2, 1}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 999}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'UPSERT with SK triggers vy_get, '
                         .. 'must conflict')
    end)
end

-- REPLACE with on_replace trigger triggers vy_get for
-- old tuple (passed to trigger).
g.test_nonblind_replace_with_trigger = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'init'}
        box.snapshot()

        -- Dummy trigger that forces old tuple lookup.
        local trigger_called = false
        s:on_replace(function() trigger_called = true end)

        box.begin()
        s:replace{1, 'new'}
        t.assert(trigger_called)

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 'concurrent'}
            ch:put(true)
        end)
        ch:get()

        local ok = pcall(box.commit)
        s:on_replace(nil)
        if not ok then pcall(box.rollback) end
        t.assert_not(ok, 'REPLACE with trigger triggers vy_get, '
                         .. 'must conflict')
    end)
end

-- A frozen-read-view scan keeps a stale value or gap out of the tuple
-- cache by flagging every skipped invisible statement (skipped_invisible),
-- which the read iterator turns into a "do not cache" verdict. That flag is
-- set in several places, and the place depends on both the iterator type
-- (forward vs reverse find_lsn take different branches) and where the
-- skipped statement lives relative to the resolved result:
--
--   same source as the result   -> find_lsn skips it (mem: M_top / M_rev;
--                                   run: R_top / R_rev)
--   a different, unscanned source -> the read iterator flags the whole
--                                   source (RI_*_notvis)
--
-- The matrix below drives every iterator type past a shadowing invisible
-- statement in each of those locations and checks that a later latest read
-- sees the new data, never a cached stale value. It runs all cells on one
-- space and one snapshot transaction per placement to stay fast.
--
-- Two skip kinds are covered: a newer invisible version of the resolved key
-- (value staleness) and an invisible key stepped over in a traversed gap
-- (gap staleness). Reverse value cells are run with one and two visible
-- versions, which take different find_lsn sub-paths.
local function skip_invisible_matrix(place)
    local fiber = require('fiber')

    -- Each cell gets a disjoint key window so cells never interfere.
    local cells = {}
    local base = 0
    local function add(kind, it, nver)
        base = base + 100
        cells[#cells + 1] = {kind = kind, it = it, nver = nver, base = base}
    end
    for _, it in ipairs({'GE', 'GT', 'EQ', 'LE', 'LT', 'REQ'}) do
        local rev = (it == 'LE' or it == 'LT' or it == 'REQ')
        for _, nver in ipairs(rev and {1, 2} or {1}) do
            add('value', it, nver)
        end
    end
    for _, it in ipairs({'GE', 'GT', 'LE', 'LT'}) do
        add('gap', it, 1)
    end

    local s = box.schema.space.create('test', {engine = 'vinyl'})
    s:create_index('pk')
    -- Visible data. value: key base+5 with nver versions. gap: boundary
    -- keys base+4 and base+6 with base+5 the (later, invisible) gap key.
    for _, c in ipairs(cells) do
        if c.kind == 'value' then
            for i = 1, c.nver do s:replace{c.base + 5, i} end
        else
            s:replace{c.base + 4}
            s:replace{c.base + 6}
        end
    end

    -- Where the shadow ends up relative to the result:
    --   same_mem    - both in the active mem            (find_lsn, mem)
    --   mem_vs_run  - result on a run, shadow in the mem (RI_*_notvis, mem)
    --   same_run    - both on one run                   (find_lsn, run)
    --   diff_run    - result on an older run, shadow on a newer run
    --                                                   (RI_*_notvis, run)
    local snap_before = (place == 'diff_run' or place == 'mem_vs_run')
    local snap_after = (place == 'same_run' or place == 'diff_run')

    if snap_before then box.snapshot() end

    box.begin({txn_isolation = 'snapshot'})
    s:get{1} -- pin the read view before the shadows are written

    local ch = fiber.channel(1)
    fiber.create(function()
        for _, c in ipairs(cells) do
            if c.kind == 'value' then
                s:replace{c.base + 5, 'vnew'} -- newer invisible version
            else
                s:replace{c.base + 5}         -- invisible key in the gap
            end
        end
        if snap_after then box.snapshot() end
        ch:put(true)
    end)
    ch:get()

    -- Scan every cell under the pinned view. value: resolve base+5
    -- (base+4/base+6 are absent). gap: return both boundary keys, stepping
    -- over the invisible base+5 between them.
    for _, c in ipairs(cells) do
        local k = c.base + 5
        local sk, lim
        if c.kind == 'value' then
            sk = ({GE = k - 1, GT = k - 1, EQ = k,
                   LE = k + 1, LT = k + 1, REQ = k})[c.it]
            lim = 1
        else
            sk = ({GE = c.base + 4, GT = c.base + 3,
                   LE = c.base + 6, LT = c.base + 7})[c.it]
            lim = 2
        end
        s:select({sk}, {iterator = c.it, limit = lim})
    end
    box.commit()

    -- A latest read must reflect the newer data, never a cached stale value.
    local fails = {}
    for _, c in ipairs(cells) do
        local nm = ('%s/%s/n%d'):format(c.kind, c.it, c.nver)
        if c.kind == 'value' then
            local got = s:get{c.base + 5}
            if not got or got[2] ~= 'vnew' then
                fails[#fails + 1] = nm .. ' value=' ..
                    require('json').encode(got)
            end
        else
            local got = s:select({c.base + 4}, {iterator = 'GE', limit = 3})
            if #got ~= 3 or got[2][1] ~= c.base + 5 then
                fails[#fails + 1] = nm .. ' gap=' ..
                    require('json').encode(got)
            end
        end
    end
    s:drop()
    return fails
end

for _, place in ipairs({'same_mem', 'mem_vs_run', 'same_run', 'diff_run'}) do
    g['test_skip_invisible_' .. place] = function(cg)
        local fails = cg.server:exec(skip_invisible_matrix, {place})
        t.assert_equals(fails, {}, place .. ' cached a stale value or gap')
    end
end
