local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
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
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

--
-- Disjoint dumps in a range that exceeds range_size should trigger
-- a split even though trim reduces the compaction plan to a single
-- slice.
--
g.test_disjoint_slices_trigger_split = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 10,
            range_size = 10 * 1024,
        })

        -- Two disjoint dumps, each ~11 KB.  Total ~22 KB exceeds
        -- range_size * 3/2 = 15 KB, so the range should be split.
        for k = 1, 100 do s:replace({k, string.rep('a', 100)}) end
        box.snapshot()

        for k = 200, 300 do s:replace({k, string.rep('b', 100)}) end
        box.snapshot()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_gt(s.index.pk:stat().range_count, 1)
        end)

        t.assert_equals(#s:select(), 201)
        t.assert_equals(s:get(1):totable(), {1, string.rep('a', 100)})
        t.assert_equals(s:get(200):totable(),
                        {200, string.rep('b', 100)})
    end)
end

--
-- Empty ranges (where last-level compaction dropped all data)
-- should be coalesced with a neighbor.
--
g.test_empty_range_coalesced = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 512,
            range_size = 4096,
        })

        -- Step 1: Create 2 dump runs spanning keys 1..40 and
        -- compact them.  This sets n_compactions = 1, which is
        -- required for the median split heuristic in Step 2.
        for i = 1, 40 do s:replace{i, string.rep('x', 200)} end
        box.snapshot()
        for i = 1, 40 do s:replace{i, string.rep('y', 200)} end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(s.index.pk:stat().run_count, 1)
        end)

        -- Step 2: Add a dump to trigger a split.
        for i = 1, 40 do s:replace{i, string.rep('z', 200)} end
        box.snapshot()
        t.helpers.retrying({timeout = 10}, function()
            t.assert_ge(s.index.pk:stat().range_count, 2)
        end)
        local range_count = s.index.pk:stat().range_count

        -- Step 3: Delete half the key space.  After compaction
        -- the affected range becomes empty and is coalesced.
        for i = 1, 20 do s:delete{i} end
        box.snapshot()
        s.index.pk:compact()

        t.helpers.retrying({timeout = 10}, function()
            t.assert_lt(s.index.pk:stat().range_count,
                        range_count)
        end)
    end)
end

--
-- Small non-empty ranges should be coalesced (the original
-- size-based coalesce path, not the empty-range path).
--
g.test_small_range_coalesced = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 512,
            range_size = 4096,
        })

        -- Step 1: Create 2 dump runs spanning keys 1..40 and
        -- compact them.  This sets n_compactions = 1, which is
        -- required for the median split heuristic in Step 2.
        for i = 1, 40 do s:replace{i, string.rep('x', 200)} end
        box.snapshot()
        for i = 1, 40 do s:replace{i, string.rep('y', 200)} end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(s.index.pk:stat().run_count, 1)
        end)

        -- Step 2: Add a dump to trigger a split.
        for i = 1, 40 do s:replace{i, string.rep('z', 200)} end
        box.snapshot()
        t.helpers.retrying({timeout = 10}, function()
            t.assert_ge(s.index.pk:stat().range_count, 2)
        end)
        local range_count = s.index.pk:stat().range_count

        -- Step 3: Delete most data, leaving only 1 key.  After
        -- compaction, each range is well below range_size / 2.
        for i = 1, 39 do s:delete{i} end
        box.snapshot()
        s.index.pk:compact()

        t.helpers.retrying({timeout = 10}, function()
            t.assert_lt(s.index.pk:stat().range_count,
                        range_count)
        end)

        -- Verify remaining data is intact.
        t.assert_equals(s:select(), {{40, string.rep('z', 200)}})
    end)
end

--
-- When trim excludes the oldest (disjoint) slice from the compaction
-- plan, is_last_level must be set to true so that tombstones are
-- dropped during compaction.
--
-- Data layout (3 runs in a single range):
--   top:    keys 1-50,   50 DELETE statements
--   middle: keys 1-100, 100 REPLACE statements
--   bottom: keys 1001-1100, 100 REPLACE statements (disjoint)
--
-- Overlap clusters: {top, middle} (size 2) and {bottom} (size 1).
-- Trim picks {top, middle}.  The original plan included all 3
-- slices (bottom = oldest).  After trim, bottom is excluded.
-- Compaction must treat this as last-level and drop tombstones.
--
g.test_trim_sets_is_last_level = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
        })

        local pad = string.rep('x', 100)

        -- Bottom run: keys 1001-1100 (disjoint from the rest).
        for k = 1001, 1100 do s:replace({k, pad}) end
        box.snapshot()

        -- Middle run: keys 1-100.
        for k = 1, 100 do s:replace({k, pad}) end
        box.snapshot()

        -- Top run: DELETE keys 1-50.
        for k = 1, 50 do s:delete({k}) end
        box.snapshot()

        -- Force compaction of all 3 slices.  The last-level
        -- invariant (>1 run at the bottom level) schedules all
        -- slices.  Trim keeps only {top, middle}.
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(s.index.pk:stat().run_count, 2)
        end)

        -- Compacted run: 50 rows (keys 51-100; tombstones for
        -- 1-50 dropped because is_last_level = true).
        -- Bottom run: 100 rows (keys 1001-1100).
        -- Total: 150 rows.
        -- With the bug (is_last_level = false): 200 rows
        -- because 50 tombstones would be preserved.
        t.assert_equals(s.index.pk:stat().disk.statement.inserts +
                         s.index.pk:stat().disk.statement.replaces +
                         s.index.pk:stat().disk.statement.deletes +
                         s.index.pk:stat().disk.statement.upserts, 150)
    end)
end

--
-- When compact() is called on an index with two disjoint overlap
-- clusters of equal size, trim picks one cluster per compaction
-- round.  The needs_compaction flag must persist so the second
-- cluster is compacted in the next round.
--
-- Data layout (5 runs in a single range):
--   run5: keys 1-100     (cluster A with run4)
--   run4: keys 1-100     (cluster A with run5)
--   run3: keys 501-600   (cluster B with run2)
--   run2: keys 501-600   (cluster B with run3)
--   run1: keys 2001-2100 (anchor, disjoint, ~10x larger)
--
-- The anchor run is large enough to sit alone at the last
-- level, keeping level_run_count = 1 and preventing
-- auto-compaction of the data runs above.
--
-- compact() sets needs_compaction; trim picks one size-2
-- cluster per round.  After the first round, needs_compaction
-- persists and the second cluster is also compacted.
--
g.test_compact_both_disjoint_clusters = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
        })

        local pad = string.rep('x', 100)
        local big_pad = string.rep('y', 1000)

        -- Anchor run: large disjoint run at the last level.
        for k = 2001, 2100 do s:replace({k, big_pad}) end
        box.snapshot()

        -- Cluster B (older): two runs on keys 501-600.
        for k = 501, 600 do s:replace({k, pad}) end
        box.snapshot()
        for k = 501, 600 do s:replace({k, pad}) end
        box.snapshot()

        -- Cluster A (newer): two runs on keys 1-100.
        for k = 1, 100 do s:replace({k, pad}) end
        box.snapshot()
        for k = 1, 100 do s:replace({k, pad}) end
        box.snapshot()

        t.assert_equals(s.index.pk:stat().run_count, 5)

        -- Trigger compaction.  Trim should compact one cluster
        -- per round, but needs_compaction persists and the
        -- scheduler eventually compacts the other cluster too.
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(s.index.pk:stat().run_count, 3)
        end)

        -- anchor + compacted A + compacted B → 300 rows.
        t.assert_equals(#s:select(), 300)
    end)
end

--
-- When level-based compaction selects small runs at level 0 and
-- trim finds them all disjoint (each forming a single-element
-- cluster), the compaction plan is dropped.  This means deferred
-- DELETEs for overlapping data at a deeper level are never
-- generated, leaving stale entries in secondary indexes.
--
-- Demonstrates the problem for REPLACE + REPLACE (keys 1-5 are
-- overwritten with new sk values) and REPLACE + DELETE (keys 6-10
-- are deleted).
--
-- Data layout (4 pk runs in a single range):
--   Run D (newest): keys 6001-6010 (disjoint)
--   Run C:          keys 5001-5010 (disjoint)
--   Run B:          keys 1-5 REPLACE + keys 6-10 DELETE
--   Run A (oldest): keys 1-100 (large, sits at a deep level)
--
-- Level 0 has B, C, D (all small, disjoint).  Shape-based
-- compaction selects them.  Trim drops the plan.  B is never
-- compacted against A, so deferred DELETEs for the overwritten
-- keys in A are never generated.
--
g.test_trim_drops_deferred_delete = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true,
        })
        local pk = s:create_index('pk', {
            run_count_per_level = 1,
            run_size_ratio = 2,
        })
        s:create_index('sk', {
            parts = {2, 'unsigned'},
            unique = false,
        })
        local pad = string.rep('x', 500)

        -- Run A (oldest, large): 100 keys.
        for i = 1, 100 do s:replace{i, i, pad} end
        box.snapshot()

        -- Run B: overwrite keys 1-5 with new sk values (REPLACE)
        -- and delete keys 6-10 (DELETE).  Both statement types get
        -- the VY_STMT_DEFERRED_DELETE flag in the pk because the
        -- old tuples are on disk and vy_point_lookup_mem finds
        -- nothing at commit time.
        for i = 1, 5 do s:replace{i, i + 1000, pad} end
        for i = 6, 10 do s:delete{i} end
        box.snapshot()

        -- Runs C and D: disjoint from B (and from each other).
        for i = 5001, 5010 do s:replace{i, i, pad} end
        box.snapshot()
        for i = 6001, 6010 do s:replace{i, i, pad} end
        box.snapshot()

        -- pk: 4 runs.  Trim dropped the compaction plan because
        -- B, C, D are all disjoint at level 0.
        t.assert_equals(pk:stat().run_count, 4)
        t.assert_equals(pk:stat().disk.compaction.count, 0)
    end)
end

--
-- UPDATE always looks up the old tuple from disk (via vy_get)
-- and generates a DELETE + REPLACE for each secondary index
-- immediately at statement time (vy_perform_update).  This is
-- not affected by the deferred DELETE mechanism or by trim.
--
g.test_update_immediate_sk_delete = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true,
        })
        s:create_index('pk')
        local sk = s:create_index('sk', {
            parts = {2, 'unsigned'},
            unique = false,
        })

        for i = 1, 10 do s:replace{i, i} end
        box.snapshot()

        -- UPDATE changes sk value (field 2).  vy_perform_update
        -- writes DELETE{old_sk} + REPLACE{new_sk} to sk.
        for i = 1, 10 do s:update(i, {{'=', 2, i + 1000}}) end

        -- sk: 10 disk REPLACEs + 10 memory DELETEs +
        --     10 memory REPLACEs = 30.
        t.assert_equals(sk:stat().rows, 30)
    end)
end

--
-- UPSERT with secondary indexes always looks up the old tuple
-- from disk (via vy_get).  If found, it applies the update
-- operations and calls vy_perform_update, which generates sk
-- DELETEs immediately.  Not affected by trim.
--
g.test_upsert_immediate_sk_delete = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true,
        })
        s:create_index('pk')
        local sk = s:create_index('sk', {
            parts = {2, 'unsigned'},
            unique = false,
        })

        for i = 1, 10 do s:replace{i, i} end
        box.snapshot()

        -- UPSERT changes sk value.  vy_get finds the old tuple
        -- on disk, so vy_perform_update generates DELETE{old_sk} +
        -- REPLACE{new_sk} immediately.
        for i = 1, 10 do
            s:upsert({i, i + 1000}, {{'=', 2, i + 1000}})
        end

        -- sk: 10 disk REPLACEs + 10 memory DELETEs +
        --     10 memory REPLACEs = 30.
        t.assert_equals(sk:stat().rows, 30)
    end)
end

--
-- Dump does not produce deferred DELETEs: the write iterator's
-- deferred DELETE handler is NULL for dumps (see vy_scheduler.c,
-- vy_task_dump_new).
--
-- Producing deferred DELETEs during dumps is not feasible because
-- dumps only read from memtables.  To generate deferred DELETEs,
-- the write iterator needs both the new and old versions of a
-- key, but the old version is on disk (not in the memtable).
-- If both versions were in the same memtable, the sk DELETE was
-- already generated at commit time by vy_tx_handle_deferred_delete.
--
-- The VY_STMT_DEFERRED_DELETE flag is preserved in the dump run
-- file so that the next compaction can generate the DELETEs.
--
g.test_dump_does_not_produce_deferred_delete = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true,
        })
        local pk = s:create_index('pk', {
            run_count_per_level = 100,
            run_size_ratio = 2,
        })
        local sk = s:create_index('sk', {
            parts = {2, 'unsigned'},
            unique = false,
            run_count_per_level = 100,
            run_size_ratio = 2,
        })

        -- Anchor run: large, sits alone at the last level.
        -- Prevents the last-level heuristic (level_run_count > 1)
        -- from triggering auto-compaction of smaller runs above.
        local big_pad = string.rep('z', 1000)
        for i = 1001, 1100 do s:replace{i, i, big_pad} end
        box.snapshot()
        local dummy_rows = 100

        -- Run A: 10 rows.
        for i = 1, 10 do s:replace{i, i} end
        box.snapshot()

        -- Replace with new sk values.  At commit time,
        -- vy_point_lookup_mem finds nothing (old tuples on disk),
        -- so the DEFERRED_DELETE flag is preserved on pk REPLACEs.
        for i = 1, 10 do s:replace{i, i + 10000} end

        -- Before dump: sk = dummy + 10 (A disk) + 10 (memory).
        t.assert_equals(sk:stat().rows - dummy_rows, 20)

        -- Dump.  Handler is NULL → no deferred DELETEs generated.
        box.snapshot()

        -- sk = dummy + 20 disk rows (A + B).  No DELETEs.
        t.assert_equals(sk:stat().rows - dummy_rows, 20)

        -- Compact pk.  All 3 runs selected (needs_compaction).
        -- Trim keeps {A, B} (overlapping), drops the anchor.
        -- The compaction write iterator sees both versions of
        -- keys 1-10 and generates deferred DELETEs via the
        -- handler → 10 DELETEs written to sk memory.
        local compact_count = pk:stat().disk.compaction.count
        pk:compact()
        t.helpers.retrying({timeout = 10}, function()
            t.assert_gt(pk:stat().disk.compaction.count, compact_count)
        end)

        -- sk = dummy + 20 disk + 10 memory DELETEs = dummy + 30.
        t.assert_equals(sk:stat().rows - dummy_rows, 30)
    end)
end
