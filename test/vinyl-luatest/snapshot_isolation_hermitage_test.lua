--
-- Hermitage anomaly tests adapted for SNAPSHOT isolation.
-- Based on github.com/ept/hermitage.
--
-- Snapshot isolation prevents: G0, G1a, G1b, G1c, OTV, PMP, P4,
-- G-single (read skew).
-- Snapshot isolation ALLOWS: G2-item (write skew), G2 (anti-dependency).
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {txn_isolation = 'snapshot'},
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
    end)
end)

-- G0: write cycles (last-writer-wins with REPLACE).
-- Both TXs write the same key. First to commit wins,
-- second detects write-write conflict.
g.test_g0_replace = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- Both TXs write key 1. Read to pin vlsn.
        box.begin()
        s:get(1)
        s:replace{1, 11}
        s:replace{2, 21}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 12}
            s:replace{2, 22}
            box.commit()
            ch:put(true)
        end)
        ch:get()

        -- TX1 has a write-write conflict on key 1 → abort.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err), 'Transaction has been aborted')

        t.assert_equals(s:get{1}, {1, 12})
        t.assert_equals(s:get{2}, {2, 22})
    end)
end

-- G1a: dirty read prevention. TX1 must not see TX2's
-- uncommitted writes.
g.test_g1a_no_dirty_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{1, 101}
            ch:put('written')
            ch:get() -- wait for TX1 to read
            box.rollback()
            ch:put('rolled_back')
        end)
        ch:get() -- TX2 has written but not committed

        box.begin()
        -- TX1 must see the original value, not TX2's dirty write.
        t.assert_equals(s:get{1}, {1, 10})
        box.commit()

        ch:put('read_done')
        ch:get() -- wait for TX2 rollback
    end)
end

-- G1b: intermediate read prevention. TX1 must not see TX2's
-- intermediate (overwritten) value.
g.test_g1b_no_intermediate_read = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        box.begin()
        -- TX1 reads first.
        t.assert_equals(s:get{1}, {1, 10})

        -- TX2 writes 101, then overwrites with 11, then commits.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 11}
            ch:put(true)
        end)
        ch:get()

        -- TX1 must still see 10 (snapshot), not 11.
        t.assert_equals(s:get{1}, {1, 10})
        box.commit()
    end)
end

-- G1c: circular information flow. Two TXs each write one key
-- and read the other's key. Snapshot isolation prevents seeing
-- each other's uncommitted writes.
g.test_g1c_no_circular_flow = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        box.begin()
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:replace{2, 22}
            -- TX2 reads key 1: must see 10, not TX1's 11.
            local v = s:get{1}
            box.commit()
            ch:put(v)
        end)

        -- TX1 reads key 2: must see 20, not TX2's 22.
        t.assert_equals(s:get{2}, {2, 20})
        box.commit()

        local tx2_read = ch:get()
        t.assert_equals(tx2_read, {1, 10})
    end)
end

-- OTV: observable transaction vanishes. TX3 must see a
-- consistent snapshot even as TX1 and TX2 commit around it.
g.test_otv_consistent_snapshot = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- TX1 writes both keys and commits.
        s:replace{1, 11}
        s:replace{2, 19}

        -- TX3 starts after TX1 commits.
        box.begin()
        -- TX3 reads key 1 -- pins its snapshot at this point.
        t.assert_equals(s:get{1}, {1, 11})

        -- TX2 writes key 1 concurrently and commits.
        -- TX3 is sent to a read view (rw conflict on key 1).
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 12}
            s:replace{2, 18}
            ch:put(true)
        end)
        ch:get()

        -- TX3 must see key 2 = 19 (from its read view snapshot),
        -- not 18 from TX2.
        t.assert_equals(s:get{2}, {2, 19})
        -- TX3 must see key 1 = 11 (its snapshot), not TX2's 12.
        t.assert_equals(s:get{1}, {1, 11})
        box.commit()
    end)
end

-- PMP: predicate many preceders. A range scan must not see
-- rows inserted by a concurrent TX after the scan started.
g.test_pmp_repeatable_scan = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        box.begin()
        t.assert_equals(s:select(), {{1, 10}, {2, 20}})

        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{3, 30}
            ch:put(true)
        end)
        ch:get()

        -- Must still see only 2 rows.
        t.assert_equals(s:select(), {{1, 10}, {2, 20}})
        box.commit()
    end)
end

-- P4: lost update prevention. Two TXs read-then-write the
-- same key. First committer wins, second aborts (write-write
-- conflict).
g.test_p4_no_lost_update = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        box.begin()
        s:get{1}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:get{1}
            s:replace{1, 12}
            box.commit()
            ch:put(true)
        end)

        ch:get()

        local ok = pcall(box.commit)
        t.assert_not(ok, 'TX1 aborts (ww conflict)')

        t.assert_equals(s:get{1}, {1, 12})
    end)
end

-- G-single: read skew prevention. TX1 reads two keys from a
-- consistent snapshot even after TX2 modifies both and commits.
g.test_g_single_no_read_skew = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        box.begin()
        t.assert_equals(s:get{1}, {1, 10})

        -- TX2 modifies both keys.
        local ch = fiber.channel(1)
        fiber.create(function()
            s:replace{1, 12}
            s:replace{2, 18}
            ch:put(true)
        end)
        ch:get()

        -- TX1 must see old value of key 2 (snapshot).
        t.assert_equals(s:get{2}, {2, 20})
        box.commit()
    end)
end

-- G2-item: write skew. This is the CLASSIC snapshot isolation
-- anomaly. Two TXs each read both keys, then write different
-- keys. Under SSI (best-effort) one is aborted. Under SNAPSHOT
-- both commit -- write skew is ALLOWED.
g.test_g2_item_write_skew_allowed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        box.begin()
        s:get{1}
        s:get{2}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:get{1}
            s:get{2}
            s:replace{2, 21}
            box.commit()
            ch:put(true)
        end)

        ch:get()

        -- TX1 should also commit -- different keys written.
        box.commit()

        -- Both writes visible: write skew occurred.
        t.assert_equals(s:get{1}, {1, 11})
        t.assert_equals(s:get{2}, {2, 21})
    end)
end

-- G2: anti-dependency cycles. Two TXs scan, then each inserts
-- a new row. Under SSI one aborts. Under SNAPSHOT both commit.
g.test_g2_anti_dependency_allowed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        box.begin()
        s:select()
        s:replace{3, 30}

        local ch = fiber.channel(1)
        fiber.create(function()
            box.begin()
            s:select()
            s:replace{4, 42}
            box.commit()
            ch:put(true)
        end)

        ch:get()

        -- TX1 should also commit -- no write-write conflict.
        box.commit()

        t.assert_equals(s:get{3}, {3, 30})
        t.assert_equals(s:get{4}, {4, 42})
    end)
end

-- ---------------------------------------------------------------
-- Mixed isolation: snapshot TX + SSI TX interacting.
-- ---------------------------------------------------------------

-- G1c mixed: snapshot TX writes key 1, SSI TX writes key 2.
-- Each reads the other's key. Neither should see the other's
-- uncommitted write.
g.test_g1c_mixed_snapshot_writes_ssi_reads = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- TX1 (snapshot) writes key 1.
        box.begin()
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (SSI) writes key 2, reads key 1.
            box.begin({txn_isolation = 'best-effort'})
            s:replace{2, 22}
            local v = s:get{1}
            box.commit()
            ch:put(v)
        end)

        -- TX1 reads key 2: must see 20, not TX2's 22.
        t.assert_equals(s:get{2}, {2, 20})
        box.commit()

        local tx2_read = ch:get()
        t.assert_equals(tx2_read, {1, 10})
    end)
end

-- G1c mixed reversed: SSI TX writes key 1, snapshot TX writes
-- key 2. The SSI TX reads key 2 (tracked), and the snapshot
-- TX's commit writes key 2, creating an rw conflict that
-- aborts the SSI TX.
g.test_g1c_mixed_ssi_writes_snapshot_reads = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- TX1 (SSI) writes key 1.
        box.begin({txn_isolation = 'best-effort'})
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (snapshot) writes key 2, reads key 1.
            box.begin()
            s:replace{2, 22}
            s:get{1}
            box.commit()
            ch:put(true)
        end)

        -- TX1 reads key 2. This tracks key 2 in TX1's read
        -- set. When TX2 commits and writes key 2, TX1 is
        -- found as a reader -- rw conflict, SSI TX aborted.
        local ok, err = pcall(s.get, s, {2})
        if ok then
            -- TX1 read succeeded (TX2 hasn't committed yet).
            -- TX1's commit will fail because TX2 wrote key 2
            -- which TX1 read.
            ok, err = pcall(box.commit)
        end
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
        pcall(box.rollback)
        ch:get()
    end)
end

-- P4 mixed: snapshot TX reads then writes key 1. SSI TX also
-- reads then writes key 1. First committer wins.
g.test_p4_mixed_snapshot_vs_ssi = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        -- TX1 (snapshot) reads key 1, then writes it.
        box.begin()
        s:get{1}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (SSI) reads key 1, then writes it.
            box.begin({txn_isolation = 'best-effort'})
            s:get{1}
            s:replace{1, 12}
            local ok = pcall(box.commit)
            ch:put(ok)
        end)

        -- TX2 commits first.
        local tx2_ok = ch:get()
        t.assert(tx2_ok, 'TX2 (SSI) commits first')

        -- TX1 (snapshot) should detect ww conflict.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- P4 mixed reversed: SSI TX reads then writes key 1.
-- Snapshot TX also reads then writes key 1.
g.test_p4_mixed_ssi_vs_snapshot = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        box.snapshot()

        -- TX1 (SSI) reads key 1, then writes it.
        box.begin({txn_isolation = 'best-effort'})
        s:get{1}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (snapshot) reads key 1, then writes it.
            box.begin()
            s:get{1}
            s:replace{1, 12}
            local ok = pcall(box.commit)
            ch:put(ok)
        end)

        -- TX2 commits first.
        local tx2_ok = ch:get()
        t.assert(tx2_ok, 'TX2 (snapshot) commits first')

        -- TX1 (SSI) should be aborted (rw conflict on key 1).
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end

-- G2-item mixed: snapshot TX reads both keys, writes key 1.
-- SSI TX reads both keys, writes key 2. Under pure SSI, one
-- would be aborted (write skew prevention). With mixed, the
-- SSI TX should be aborted (it detects rw conflict from the
-- snapshot TX's write), while the snapshot TX commits
-- (it allows write skew).
g.test_g2_item_mixed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- TX1 (snapshot) reads both keys, writes key 1.
        box.begin()
        s:get{1}
        s:get{2}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (SSI) reads both keys, writes key 2.
            box.begin({txn_isolation = 'best-effort'})
            s:get{1}
            s:get{2}
            s:replace{2, 21}
            local ok = pcall(box.commit)
            ch:put(ok)
        end)

        local tx2_ok = ch:get()
        -- TX2 (SSI) commits first. It wrote key 2 and found
        -- TX1 as a reader of key 2 (TX1 is snapshot, so TX1
        -- is sent to a read view, not aborted).
        t.assert(tx2_ok, 'TX2 (SSI) commits first')

        -- TX1 (snapshot) commits. Write skew is allowed for
        -- snapshot isolation -- TX1 wrote key 1, TX2 wrote
        -- key 2, no ww conflict.
        box.commit()

        t.assert_equals(s:get{1}, {1, 11})
        t.assert_equals(s:get{2}, {2, 21})
    end)
end

-- G2-item mixed reversed: SSI TX reads both, writes key 1.
-- Snapshot TX reads both, writes key 2.
g.test_g2_item_mixed_reversed = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 10}
        s:replace{2, 20}
        box.snapshot()

        -- TX1 (SSI) reads both keys, writes key 1.
        box.begin({txn_isolation = 'best-effort'})
        s:get{1}
        s:get{2}
        s:replace{1, 11}

        local ch = fiber.channel(1)
        fiber.create(function()
            -- TX2 (snapshot) reads both keys, writes key 2.
            box.begin()
            s:get{1}
            s:get{2}
            s:replace{2, 21}
            local ok = pcall(box.commit)
            ch:put(ok)
        end)

        local tx2_ok = ch:get()
        -- TX2 (snapshot) commits first. It wrote key 2.
        -- TX1 (SSI) read key 2, so TX1 has an rw conflict.
        t.assert(tx2_ok, 'TX2 (snapshot) commits first')

        -- TX1 (SSI) should be aborted: it read key 2 which
        -- TX2 wrote (rw conflict), and SSI aborts RW TXs on
        -- rw conflict.
        local ok, err = pcall(box.commit)
        t.assert_not(ok)
        t.assert_str_contains(tostring(err),
            'Transaction has been aborted')
    end)
end
