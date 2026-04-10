local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
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
-- Bloat compaction: after a range split, two ranges share one run
-- file via separate slices.  When one range compacts its slice,
-- the other range's slice pins unreferenced data.  The bloat
-- detection should compact that slice too.
--
-- The shared run here is the original compacted run that was
-- split into two ranges.  To consume its slice, we dump a small
-- overlapping set to one range (placing it at a different level
-- than the large original) and then use compact() to force
-- compaction of all slices via needs_compaction.  The other
-- range has only 1 slice, so compact() is a no-op for it.
-- After the first range compacts, bloat propagation fires for
-- the second range.
--
-- The small dump at a different level avoids triggering auto-
-- compaction regardless of the level_count > 1 guard: each
-- level has exactly 1 run, so level_run_count never exceeds 1.
--
g.test_bloat_compaction_after_split = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- 80 rows x 200 bytes = 17 KB per dump. page_size=128
        -- gives 1 row per page (80 pages, 5 LCP groups).
        -- range_size=8192 ensures exactly 1 median split
        -- after compaction (17 KB > 2x8192).
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 128,
            range_size = 8192,
        })

        -- Phase 1: Dump keys 1..80 twice, then force-compact.
        -- compact() merges the 2 runs into one.  The merged run
        -- (~17 KB) exceeds range_size, triggering a proactive
        -- split into 2 ranges that share the merged run.
        for i = 1, 80 do s:replace{i, string.rep('x', 200)} end
        box.snapshot()
        for i = 1, 80 do s:replace{i, string.rep('y', 200)} end
        box.snapshot()
        s.index.pk:compact()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
            t.assert_equals(s.index.pk:stat().range_count, 2)
            t.assert_equals(s.index.pk:stat().run_count, 1)
        end)

        -- Phase 2: Dump a small overlapping set (~5 keys × 200
        -- bytes ≈ 1 KB) to one range.  The dump is ~10× smaller
        -- than the original per-range slice (~8.5 KB), placing
        -- it at a different level.  compact() sets needs_compaction
        -- on both ranges, but range 2 has only 1 slice (the
        -- original's slice), so its plan stays empty.  Range 1
        -- compacts (needs_compaction selects all 2 slices),
        -- consuming its slice of the shared run.  Bloat
        -- propagation then fires for range 2.
        local compaction_before =
            s.index.pk:stat().disk.compaction.count
        for i = 1, 5 do s:replace{i, string.rep('z', 200)} end
        box.snapshot()
        s.index.pk:compact()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_ge(s.index.pk:stat().disk.compaction.count,
                        compaction_before + 2)
        end)
        local stat = s.index.pk:stat()
        t.assert_equals(stat.range_count, 2)
        t.assert_equals(stat.run_count, 2)
    end)
end

--
-- Bloat propagation chain: after compacting one range that shares
-- a run, debloat propagation wakes the next idle neighbor, which
-- bloat-compacts and wakes the next, forming a chain.
--
-- Setup: 4 data ranges + 1 anchor range.  The anchor (keys
-- 10001-10100) is created first so that the rightmost data range
-- has a finite end key (not +inf).
--
--   1. Anchor dump → 5 ranges after splits: anchor + 4 data.
--   2. Shared dump spanning all 4 data ranges, 20 keys each.
--   3. Trigger compaction in range 1 (group 0, the leftmost).
--      Debloat wave goes strictly left → right: range 1 → 2
--      → 3 → 4.  Total: 1 shape + 3 bloat = 4.
--
-- Level_count independence: all runs in the target range land at
-- level 0 (level_count=1), so the level_count > 1 guard never
-- fires regardless of whether it exists.
--
g.test_bloat_propagation_across_ranges = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 128,
            range_size = 33000,
        })

        local pad = string.rep('x', 200)

        -- Anchor range: 100 keys at 10001-10100.  Dumped first
        -- so that the last data range (group 3, keys 3001-3150)
        -- gets a finite end key instead of +inf.
        for k = 10001, 10100 do s:replace{k, pad} end
        box.snapshot()

        -- Phase 1: Create 4 disjoint data runs.  Each run covers
        -- one key group (150 keys × ~215 bytes ≈ 33 KB ≈
        -- range_size).  Split cascade → 4 data ranges + anchor.
        for group = 0, 3 do
            for k = group * 1000 + 1, group * 1000 + 150 do
                s:replace{k, pad}
            end
            box.snapshot()
        end
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(s.index.pk:stat().range_count, 5)
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)
        -- Prevent further splits: the target range will have
        -- up to ~70 KB after trigger dumps.  Set range_size
        -- high enough to avoid splits (1.5 × 100000 = 150 KB).
        s.index.pk:alter({range_size = 100000})

        -- Phase 2: Single shared dump spanning all 4 data ranges.
        -- 20 keys × ~215 bytes per range ≈ 4.3 KB per slice.
        -- With page_size=128, the run has ~80 pages total, ~20
        -- per slice.  This is large enough for bloat detection:
        -- after one slice is removed, waste ≈ 20*20/80 = 5 ≥ 2.
        for group = 0, 3 do
            for k = group * 1000 + 1, group * 1000 + 20 do
                s:replace{k, pad}
            end
        end
        box.snapshot()

        t.helpers.retrying({timeout = 10}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)
        t.assert_equals(s.index.pk:stat().range_count, 5,
                        'no splits after overlapping dump')

        -- Phase 3: Set run_count_per_level=2.  Each data range
        -- has 2 slices: original (~33 KB) and shared (~4 KB).
        -- With level_count=1 (both at same level),
        -- level_run_count=2 does NOT exceed rcpl=2.
        s.index.pk:alter({run_count_per_level = 2})

        local compaction_before =
            s.index.pk:stat().disk.compaction.count

        -- Loop-based trigger dumps to range 1 (group 0, keys
        -- 1-150, overlapping with original).  Each dump creates
        -- a ~33 KB run at level 0.  With 3+ runs at level 0,
        -- level_run_count > rcpl=2 → shape compaction selects
        -- all slices (including shared dump's slice), consuming
        -- the shared run.  The debloat wave then propagates
        -- strictly left → right: range 2 → 3 → 4.
        for _ = 1, 10 do
            for k = 1, 150 do s:replace{k, pad} end
            box.snapshot()
            t.helpers.retrying({timeout = 5}, function()
                t.assert_equals(
                    box.stat.vinyl().scheduler.idle, 1)
            end)
            if s.index.pk:stat().disk.compaction.count
                    > compaction_before then
                break
            end
        end

        -- After shape compaction in range 1 consumes the shared
        -- run's slice, debloat propagation walks right through
        -- ranges 2 → 3 → 4.
        -- Wait for the full chain: 1 shape + 3 bloat = 4.
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_ge(s.index.pk:stat().disk.compaction.count
                        - compaction_before, 4)
        end)
        t.assert_equals(box.stat.vinyl().scheduler.tasks_failed, 0)

        -- Verify data integrity: 4 groups × 150 + anchor 100
        -- = 700.
        t.assert_equals(s:count(), 700)
    end)
end

--
-- Bidirectional debloat wave: trigger compaction from the middle
-- so the wave propagates both left and right.
--
-- Setup: 8 data ranges.  Shared dump covers only central 6
-- (ranges 2-7, groups 1-6), leaving ranges 1 and 8 (groups 0
-- and 7) without shared data.
--
-- Each range has its original run (~33 KB, 150 keys).  The shared
-- dump has 20 keys per range → ~4.3 KB per slice, ~120 pages total.
-- All slices sit at the same level (level_count=1).  Trigger
-- dumps to range 5 (group 4) push level_run_count past rcpl=2,
-- causing shape compaction that consumes the shared run's slice.
-- The wave propagates outward from range 5: forward through
-- ranges 6-7 and backward through ranges 4, 3, 2.  Ranges 1 and
-- 8 have no shared slice and are NOT debloated.
--
g.test_debloat_bidirectional_wave = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 128,
            range_size = 33000,
        })

        local pad = string.rep('x', 200)

        -- Phase 1: 8 disjoint data dumps.
        -- Groups: keys i*1000+1 .. i*1000+150 (i = 0..7).
        -- Each group: 150 keys × ~215 bytes ≈ 33 KB ≈ range_size.
        -- Split cascade → 8 ranges, 1 run each.
        for group = 0, 7 do
            for k = group * 1000 + 1, group * 1000 + 150 do
                s:replace{k, pad}
            end
            box.snapshot()
        end
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(s.index.pk:stat().range_count, 8)
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)

        -- Prevent further splits.
        s.index.pk:alter({range_size = 100000})

        -- Phase 2: Shared dump spanning only groups 1-6 (ranges
        -- 2-7).  Skip groups 0 and 7 (ranges 1 and 8).
        -- 20 keys × ~215 bytes per range ≈ 4.3 KB per slice.
        -- With page_size=128, ~120 pages total, ~20 per slice.
        for group = 1, 6 do
            for k = group * 1000 + 1, group * 1000 + 20 do
                s:replace{k, pad}
            end
        end
        box.snapshot()

        t.helpers.retrying({timeout = 10}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)

        local compaction_before =
            s.index.pk:stat().disk.compaction.count

        -- Phase 3: Set rcpl=2.  Each range with a shared dump
        -- has 2 slices at level_count=1.  level_run_count=2
        -- does not exceed rcpl=2, so no auto-compaction.
        s.index.pk:alter({run_count_per_level = 2})

        -- Loop: dump 150 overlapping keys to range 5 (group 4,
        -- keys 4001-4150).  Each dump is ~33 KB, same level as
        -- the original.  With 3+ runs at level 0 → shape
        -- compaction selects all slices including shared dump.
        for _ = 1, 10 do
            for k = 4001, 4150 do s:replace{k, pad} end
            box.snapshot()
            t.helpers.retrying({timeout = 5}, function()
                t.assert_equals(
                    box.stat.vinyl().scheduler.idle, 1)
            end)
            if s.index.pk:stat().disk.compaction.count
                    > compaction_before then
                break
            end
        end

        -- Phase 4: Wait for the bidirectional debloat wave.
        -- Range 5 (group 4): shape compaction.
        -- Forward:  ranges 6 → 7 (bloat, groups 5-6).
        -- Backward: ranges 4 → 3 → 2 (bloat, groups 3, 2, 1).
        -- Total: 1 shape + 5 bloat = 6.
        -- Ranges 1 and 8 (groups 0, 7) have no shared slice.
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_ge(s.index.pk:stat().disk.compaction.count
                        - compaction_before, 6)
        end)
        t.assert_equals(box.stat.vinyl().scheduler.tasks_failed, 0)

        -- Verify data integrity: 8 groups × 150 keys = 1200.
        t.assert_equals(s:count(), 1200)
    end)
end

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
            page_size = 128,
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
            page_size = 128,
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
-- This is acceptable because the stale SK entries will be cleaned
-- up by the read-amplification compaction driver: once enough
-- range scans accumulate wasted disk reads from the shadowed
-- versions, the read-amp threshold triggers a full compaction
-- that merges B with A and generates the deferred DELETEs.
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

--
-- Negative test: verify that a slice with waste below the bloat
-- threshold is NOT compacted.
--
-- Setup: 3 ranges from two splits sharing a small run.  After one
-- range compacts (via auto-compact), the shared run has minimal
-- waste: with ~3 pages total and ~1 page per slice,
-- unreferenced ≈ 1, waste = 1*1/3 = 0 < 2.  Bloat compaction
-- should NOT fire.
--
g.test_bloat_below_threshold = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 2,
            page_size = 128,
            range_size = 5000,
        })

        local pad = string.rep('x', 200)

        -- Phase 1: 50 keys × 2 dumps + compact.  The merged run
        -- has 50 pages, large enough for the LCP-based split to
        -- find a mid-group boundary and split into 2 ranges.
        for i = 1, 50 do s:replace{i, pad} end
        box.snapshot()
        for i = 1, 50 do s:replace{i, pad} end
        box.snapshot()
        s.index.pk:compact()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(s.index.pk:stat().range_count, 2)
        end)

        -- Phase 1b: 50 new keys × 2 dumps to the upper range.
        -- The upper range hits 3 slices and auto-compacts; the
        -- merged result splits again, producing 3 ranges total.
        for i = 51, 100 do s:replace{i, pad} end
        box.snapshot()
        for i = 51, 100 do s:replace{i, pad} end
        box.snapshot()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(box.stat.vinyl().scheduler.idle, 1)
            t.assert_equals(s.index.pk:stat().range_count, 3)
        end)

        -- Phase 2: Snapshot one key per range -- this produces
        -- a tiny shared run with one slice in each of the 3
        -- ranges. Then snapshot another key in range 1's key
        -- space -- this produces a single-slice run in range 1
        -- only. Range 1 now has 3 slices and auto-compacts;
        -- the other ranges stay at 2 slices.
        local compaction_before =
            s.index.pk:stat().disk.compaction.count
        s:replace{1, pad}
        s:replace{40, pad}
        s:replace{75, pad}
        box.snapshot()
        s:replace{2, pad}
        box.snapshot()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_gt(s.index.pk:stat().disk.compaction.count,
                        compaction_before)
        end)
        local compaction_after =
            s.index.pk:stat().disk.compaction.count

        -- Verify no additional compaction fires.  The retrying
        -- above confirmed the scheduler is idle, so no new
        -- compaction can start without a fresh trigger.
        t.assert_equals(s.index.pk:stat().disk.compaction.count,
                        compaction_after,
                        'no bloat compaction below threshold')
    end)
end

--
-- When shape compaction selects only the top slices (the oldest
-- sits at a deeper level), check_last_level detects that the
-- plan's key range doesn't overlap the older slice and sets
-- is_last_level = true.  Tombstones are pruned.
--
-- Data layout (4 slices in a single range):
--   S0 (newest): REPLACE keys 26-50                     (~5 KB)
--   S1:          DEL keys 1-25, REPLACE keys 26-50      (~6 KB)
--   S2:          REPLACE keys 1-50                      (~11 KB)
--   S3 (oldest): REPLACE keys 200-1199                  (~210 KB)
--
-- S3 is deliberately large (1000 keys) so that
-- target_run_size ~= 210K / 3.5^2 ~= 17 KB, which is bigger
-- than any of S0, S1, S2.  This puts all three at level 0
-- (level_run_count = 3).  Even with randomization (+1),
-- 3 > rcpl + 1 = 2, so shape compaction always fires and
-- selects S0 + S1 + S2, leaving S3 at a deeper level.
--
-- check_last_level: plan cluster covers keys 1-50 (from
-- run min/max keys).  S3 covers keys 200-1199.  Disjoint →
-- is_last_level = true → DELETEs for keys 1-25 are pruned.
--
g.test_trim_compaction_prunes_tombstones = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 1,
            page_size = 512,
            range_size = 1000000,
        })

        local pad = string.rep('x', 200)

        -- S3 (oldest): 1000 keys at 200-1199.  ~210 KB, sits at
        -- a deep level.  Makes target_run_size at level 0 ~17 KB
        -- so that S0/S1/S2 all fit there.
        for k = 200, 1199 do s:replace{k, pad} end
        box.snapshot()

        -- S2: 50 keys at 1-50.  ~11 KB, level 0.
        for k = 1, 50 do s:replace{k, pad} end
        box.snapshot()

        -- S1: DELETE keys 1-25, REPLACE keys 26-50.  ~6 KB.
        for k = 1, 25 do s:delete{k} end
        for k = 26, 50 do s:replace{k, pad} end
        box.snapshot()

        -- S0 (newest): REPLACE keys 26-50.  ~5 KB.
        -- After this dump we have 4 runs with level_run_count = 3
        -- at level 0 (S0 + S1 + S2), which exceeds rcpl + 1 = 2
        -- even with randomization.  Shape compaction selects
        -- S0 + S1 + S2.
        for k = 26, 50 do s:replace{k, pad} end
        box.snapshot()

        -- Wait for shape compaction to complete.
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_equals(s.index.pk:stat().run_count, 2)
        end)

        -- After compaction: 2 runs remain (compacted S0+S1+S2,
        -- and S3).  Tombstones for keys 1-25 should be pruned
        -- (is_last_level = true via check_last_level).
        local stat = s.index.pk:stat()
        t.assert_equals(stat.disk.statement.deletes, 0,
                        'tombstones pruned by is_last_level')
        -- 25 keys (26-50) + 1000 keys (200-1199) = 1025.
        t.assert_equals(s:count(), 1025)
    end)
end

--
-- Bloat compaction with check_last_level: a shared dump puts
-- data into a gap region of range 1 (beyond its older run's
-- max_key).  Bloat compaction picks the shared slice; since it
-- is disjoint from the older run, check_last_level sets
-- is_last_level = true and tombstones are pruned.
--
-- Reuses the 4-range setup from test_bloat_propagation_across_ranges.
-- Range 1 (leftmost, keys 1-150) has an older run with max_key ≈ 150.
-- The shared dump writes to keys 500-549 in range 1's gap region
-- (between its data and the range boundary).  25 DELETEs + 25
-- REPLACEs.  Bloat compaction picks the shared slice.  The shared
-- run's global min_key = 500, max_key ≈ 3020.  Older run's
-- min_key = 1, max_key = 150.  150 < 500 → disjoint →
-- is_last_level = true → tombstones pruned.
--
g.test_bloat_compaction_prunes_tombstones = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 128,
            range_size = 33000,
        })

        local pad = string.rep('x', 200)

        -- Phase 1: Create 4 disjoint data ranges (same as
        -- test_bloat_propagation_across_ranges).
        for group = 0, 3 do
            for k = group * 1000 + 1, group * 1000 + 150 do
                s:replace{k, pad}
            end
            box.snapshot()
        end
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(s.index.pk:stat().range_count, 4)
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)
        s.index.pk:alter({range_size = 100000})

        -- Phase 2: Shared dump.
        -- Range 1 (group 0): write to gap region (keys 500-549).
        -- First replace keys 500-549, then delete keys 500-524.
        -- In the memtable, deletes supersede replaces for 500-524.
        -- The dump run has: DEL 500-524, REPL 525-549.
        -- DELETEs are preserved in the dump.
        for k = 500, 549 do s:replace{k, pad} end
        for k = 500, 524 do s:delete{k} end
        -- Ranges 2-4: 20 overlapping keys each (same as propagation).
        for group = 1, 3 do
            for k = group * 1000 + 1, group * 1000 + 20 do
                s:replace{k, pad}
            end
        end
        box.snapshot()

        t.helpers.retrying({timeout = 10}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
        end)

        -- Phase 3: Trigger shape compaction → debloat wave.
        s.index.pk:alter({run_count_per_level = 2})
        local compaction_before =
            s.index.pk:stat().disk.compaction.count

        -- Trigger compaction in range 4 (group 3).
        for _ = 1, 10 do
            for k = 3001, 3150 do s:replace{k, pad} end
            box.snapshot()
            t.helpers.retrying({timeout = 5}, function()
                t.assert_equals(
                    box.stat.vinyl().scheduler.idle, 1)
            end)
            if s.index.pk:stat().disk.compaction.count
                    > compaction_before then
                break
            end
        end

        -- Wait for the full debloat chain: range 4 → 3 → 2 → 1.
        t.helpers.retrying({timeout = 30}, function()
            t.assert_equals(
                box.stat.vinyl().scheduler.idle, 1)
            t.assert_ge(s.index.pk:stat().disk.compaction.count
                        - compaction_before, 4)
        end)
        t.assert_equals(box.stat.vinyl().scheduler.tasks_failed, 0)

        -- In range 1, bloat compaction picked the shared slice
        -- (keys 500-549).  Older run (keys 1-150, max_key=150)
        -- is disjoint → is_last_level → tombstones pruned.
        t.assert_equals(s.index.pk:stat().disk.statement.deletes, 0,
                        'tombstones pruned by bloat is_last_level')
        -- 4 groups × 150 + 25 remaining REPLACEs from shared dump.
        t.assert_equals(s:count(), 625)
    end)
end

--
-- Read amplification compaction: when tombstones in a newer run
-- shadow inserts in an older run, range scans accumulate wasted
-- disk reads (bytes decompressed only to be discarded).  Once the
-- cumulative waste exceeds the range's data size, a compaction is
-- triggered that merges the runs and removes the tombstones.
--
g.test_read_amp_compaction = function(cg)
    cg.server:exec(function()
        -- Disable the vinyl cache so range scans always reach
        -- disk and the waste counters accumulate on every scan.
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })

        -- Step 1: Insert 10 keys (~1KB each) and snapshot → run R1.
        for i = 1, 10 do s:replace{i, string.rep('x', 1000)} end
        box.snapshot()
        t.assert_equals(s.index.pk:stat().run_count, 1)

        -- Step 2: Delete all 10 keys and snapshot → run R2
        -- (tombstones).  Now the range has 2 runs.  Shape-based
        -- compaction doesn't fire (run_count_per_level = 100).
        for i = 1, 10 do s:delete{i} end
        box.snapshot()
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Step 3: Full-range scans.  For each of the 10 keys
        -- the merge iterator reads both INSERT (from R1) and
        -- DELETE (from R2) off disk, only to discard the result.
        -- bytes_read grows while bytes_useful stays 0.  After
        -- ~5 scans the waste exceeds VY_READ_AMP_THRESHOLD *
        -- range->count.bytes (threshold is 4x), which triggers
        -- compaction automatically via the read-amp callback.
        for _ = 1, 10 do s:select() end

        -- Wait for the read-amp-triggered compaction to complete.
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- After compaction the tombstones are gone (last-level
        -- compaction drops DELETEs).  The space is empty.
        t.assert_equals(s:select(), {})

        s:drop()
        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Read amplification compaction triggered by point lookups.
-- Same setup as test_read_amp_compaction (tombstones shadowing
-- inserts), but uses get() instead of select().
--
-- Point lookups charge tuple_size per disk statement — the same
-- metric as range scans.  This captures redundancy (tombstones,
-- shadowed versions) without inflating waste with the inherent
-- page-read cost that compaction cannot eliminate.
--
-- With small DELETE tuples (~60 bytes each), each get() on a
-- deleted key charges only tuple_size(DELETE) as waste.  We use
-- 100 keys so that the per-round waste (100 × ~60 = ~6 KB)
-- accumulates to exceed VY_READ_AMP_THRESHOLD *
-- range->count.bytes (4 × ~10 KB = ~40 KB) in ~7 rounds.
--
g.test_read_amp_point_lookup = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
        })

        -- Step 1: Insert 100 keys (~100B each) and snapshot → R1.
        for i = 1, 100 do s:replace{i, string.rep('x', 100)} end
        box.snapshot()

        -- Step 2: Delete all 100 keys and snapshot → R2.
        for i = 1, 100 do s:delete{i} end
        box.snapshot()
        t.assert_equals(s.index.pk:stat().run_count, 2)

        -- Step 3: Point lookups on deleted keys.  Each get()
        -- finds a DELETE in R2 (terminal) and returns nil.
        -- Waste per get = tuple_size(DELETE) ≈ 60 bytes.
        -- Per round: 100 × 60 = ~6 KB.
        -- range->count.bytes ≈ 10 KB. Threshold is 4x = 40 KB.
        -- ~7 rounds should exceed the threshold; use 60 for
        -- margin.  Compaction is triggered automatically via
        -- the read-amp callback.
        for _ = 1, 60 do
            for i = 1, 100 do s:get{i} end
        end

        -- Wait for read-amp-triggered compaction.
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- After compaction: tombstones gone, space is empty.
        t.assert_equals(s:select(), {})

        s:drop()
        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Deferred delete read-amp: range scans on a secondary index
-- return orphaned entries (the primary was updated but hasn't
-- compacted to produce deferred deletes yet).  Each orphan
-- charges tuple_size to the primary's read-amp, eventually
-- triggering primary compaction.
--
-- Tests each DML type independently:
--   REPLACE: old SK entry stays on disk (deferred delete) → orphan.
--   DELETE:  old SK entry stays on disk (deferred delete) → orphan.
--   UPDATE:  vy_perform_update generates immediate SK DELETE →
--            no orphan, no PK compaction.
--
g.test_read_amp_deferred_delete_scan = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local n = 100
        local pad = string.rep('x', 500)

        local dml_types = {
            {
                name = 'replace',
                apply = function(s)
                    for i = 1, n do
                        s:replace{i, i + 10000, pad}
                    end
                end,
                expect_compaction = true,
                expect_count = n,
            },
            {
                name = 'delete',
                apply = function(s)
                    for i = 1, n do s:delete{i} end
                end,
                expect_compaction = true,
                expect_count = 0,
            },
            {
                name = 'update',
                apply = function(s)
                    for i = 1, n do
                        s:update(i, {{'=', 2, i + 10000}})
                    end
                end,
                expect_compaction = false,
                expect_count = n,
            },
        }

        for _, dml in ipairs(dml_types) do
            local s = box.schema.space.create('test', {
                engine = 'vinyl', defer_deletes = true,
            })
            s:create_index('pk', {run_count_per_level = 100})
            s:create_index('sk', {
                parts = {2, 'unsigned'}, unique = false,
                run_count_per_level = 100,
            })

            for i = 1, n do s:replace{i, i, pad} end
            box.snapshot()

            dml.apply(s)
            box.snapshot()

            for _ = 1, 50 do s.index.sk:select() end

            if dml.expect_compaction then
                t.helpers.retrying({timeout = 5}, function()
                    t.assert_ge(
                        s.index.pk:stat().disk.compaction.count, 1,
                        dml.name .. ': expected PK compaction')
                end)
            else
                t.assert_equals(
                    s.index.pk:stat().disk.compaction.count, 0,
                    dml.name .. ': no PK compaction expected')
            end

            t.assert_equals(s:count(), dml.expect_count,
                            dml.name .. ': row count')
            s:drop()
        end

        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Deferred delete read-amp with point lookups on a secondary
-- index.  Uses select(key, {limit=1}) on old SK values to
-- trigger orphan detection and charge waste to the primary.
-- (get() doesn't support non-unique indexes.)
--
-- Tests each DML type independently (same rationale as
-- test_read_amp_deferred_delete_scan).
--
g.test_read_amp_deferred_delete_get = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local n = 100
        local pad = string.rep('x', 500)

        local dml_types = {
            {
                name = 'replace',
                apply = function(s)
                    for i = 1, n do
                        s:replace{i, i + 10000, pad}
                    end
                end,
                expect_compaction = true,
                expect_count = n,
            },
            {
                name = 'delete',
                apply = function(s)
                    for i = 1, n do s:delete{i} end
                end,
                expect_compaction = true,
                expect_count = 0,
            },
            {
                name = 'update',
                apply = function(s)
                    for i = 1, n do
                        s:update(i, {{'=', 2, i + 10000}})
                    end
                end,
                expect_compaction = false,
                expect_count = n,
            },
        }

        for _, dml in ipairs(dml_types) do
            local s = box.schema.space.create('test', {
                engine = 'vinyl', defer_deletes = true,
            })
            s:create_index('pk', {run_count_per_level = 100})
            s:create_index('sk', {
                parts = {2, 'unsigned'}, unique = false,
                run_count_per_level = 100,
            })

            for i = 1, n do s:replace{i, i, pad} end
            box.snapshot()

            dml.apply(s)
            box.snapshot()

            -- Point-like lookups on old SK values.  For REPLACE
            -- and DELETE, each finds an orphaned SK entry.  For
            -- UPDATE, old SK entries are cancelled by immediate
            -- DELETEs in SK run 2.
            for _ = 1, 50 do
                for i = 1, n do
                    s.index.sk:select({i}, {limit = 1})
                end
            end

            if dml.expect_compaction then
                t.helpers.retrying({timeout = 5}, function()
                    t.assert_ge(
                        s.index.pk:stat().disk.compaction.count, 1,
                        dml.name .. ': expected PK compaction')
                end)
            else
                t.assert_equals(
                    s.index.pk:stat().disk.compaction.count, 0,
                    dml.name .. ': no PK compaction expected')
            end

            t.assert_equals(s:count(), dml.expect_count,
                            dml.name .. ': row count')
            s:drop()
        end

        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Deferred delete read-amp from multiple secondary indexes.
-- Each secondary individually contributes below the primary's
-- waste threshold, but together they cross it.
--
-- Tests each DML type independently (same rationale as
-- test_read_amp_deferred_delete_scan).
--
g.test_read_amp_deferred_delete_multiple_sk = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local n = 100
        local pad = string.rep('x', 500)

        local dml_types = {
            {
                name = 'replace',
                apply = function(s)
                    for i = 1, n do
                        s:replace{i, i + 10000, i + 20000, pad}
                    end
                end,
                expect_compaction = true,
                expect_count = n,
            },
            {
                name = 'delete',
                apply = function(s)
                    for i = 1, n do s:delete{i} end
                end,
                expect_compaction = true,
                expect_count = 0,
            },
            {
                name = 'update',
                apply = function(s)
                    for i = 1, n do
                        s:update(i, {{'=', 2, i + 10000},
                                     {'=', 3, i + 20000}})
                    end
                end,
                expect_compaction = false,
                expect_count = n,
            },
        }

        for _, dml in ipairs(dml_types) do
            local s = box.schema.space.create('test', {
                engine = 'vinyl', defer_deletes = true,
            })
            s:create_index('pk', {run_count_per_level = 100})
            s:create_index('sk1', {
                parts = {2, 'unsigned'}, unique = false,
                run_count_per_level = 100,
            })
            s:create_index('sk2', {
                parts = {3, 'unsigned'}, unique = false,
                run_count_per_level = 100,
            })

            for i = 1, n do s:replace{i, i, i, pad} end
            box.snapshot()

            dml.apply(s)
            box.snapshot()

            -- Scan each secondary.  Each individually contributes
            -- below the primary's waste threshold, but together
            -- they cross it.
            for _ = 1, 25 do s.index.sk1:select() end
            for _ = 1, 25 do s.index.sk2:select() end

            if dml.expect_compaction then
                t.helpers.retrying({timeout = 5}, function()
                    t.assert_ge(
                        s.index.pk:stat().disk.compaction.count, 1,
                        dml.name .. ': expected PK compaction')
                end)
            else
                t.assert_equals(
                    s.index.pk:stat().disk.compaction.count, 0,
                    dml.name .. ': no PK compaction expected')
            end

            t.assert_equals(s:count(), dml.expect_count,
                            dml.name .. ': row count')
            s:drop()
        end

        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Read-amp compaction suppression: after compaction resets the
-- stats, further scans should NOT re-trigger compaction until
-- the range's slice structure changes (version bumps).
--
g.test_read_amp_suppression_after_compaction = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 256,
        })

        -- Same setup: 10 inserts + 10 tombstones in 2 runs.
        for i = 1, 10 do s:replace{i, string.rep('x', 1000)} end
        box.snapshot()
        for i = 1, 10 do s:delete{i} end
        box.snapshot()

        -- Accumulate waste and trigger compaction.
        -- With 4x threshold, need ~5+ scans for the all-tombstone
        -- workload to exceed the threshold.
        for _ = 1, 10 do s:select() end
        s:replace{5, 'alive'}
        box.snapshot()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)
        local compaction_count = s.index.pk:stat().disk.compaction.count

        -- More scans after compaction.  Since threshold_version ==
        -- version (slices haven't changed since compaction reset
        -- the stats), compaction must NOT re-trigger.
        for _ = 1, 5 do s:select() end
        -- Insert + snapshot: this snapshot creates a new dump
        -- which bumps the range version, but the range now has
        -- only 2 runs with tombstones already removed, so waste
        -- should be minimal.
        s:replace{6, 'another'}
        box.snapshot()
        -- The snapshot yields, giving the scheduler a chance to
        -- run.  No compaction should trigger — waste is minimal.
        t.assert_equals(s.index.pk:stat().disk.compaction.count,
                        compaction_count,
                        'no re-trigger after compaction')

        s:drop()
        box.cfg{vinyl_cache = old_cache}
    end)
end

--
-- Verify that reading a no-garbage range (multiple runs, non-
-- overlapping keys, no tombstones) does NOT accumulate waste and
-- does NOT trigger read-amp compaction.
--
g.test_read_amp_no_garbage = function(cg)
    cg.server:exec(function()
        local old_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            run_count_per_level = 100,  -- suppress shape compaction
        })

        -- Create two runs with non-overlapping keys.
        -- Run 1: keys 1..50
        for i = 1, 50 do
            s:replace{i, string.rep('x', 100)}
        end
        box.snapshot()

        -- Run 2: keys 51..100
        for i = 51, 100 do
            s:replace{i, string.rep('y', 100)}
        end
        box.snapshot()

        -- Confirm we have 2 runs, no compaction yet.
        -- With 2 equal-size runs: both land at level 1,
        -- level_count=1, so the "last level > 1 run" rule
        -- doesn't fire.  And 2 < run_count_per_level=100.
        -- The read-amp check IS reachable: slice_count=2 >= 2,
        -- version=2, reset_version=0 (never compacted).
        t.assert_equals(s.index.pk:stat().run_count, 2)
        t.assert_equals(s.index.pk:stat().disk.compaction.count, 0)

        -- Heavy reads: range scans and point lookups.
        -- Each key exists in exactly one run (non-overlapping),
        -- so every disk read produces exactly one statement
        -- that becomes the result.  bytes_read == bytes_useful,
        -- waste == 0.
        for _ = 1, 30 do
            s:select()
            for i = 1, 100 do
                s:get{i}
            end
        end

        -- No compaction should have triggered — waste is ~0.
        -- The reads above yield many times (disk I/O), giving
        -- the scheduler fiber ample opportunity to run.
        t.assert_equals(s.index.pk:stat().disk.compaction.count, 0,
                        'no compaction in a no-garbage range')

        s:drop()

        -- SK with defer_deletes: verify that SK reads on a clean
        -- space (no orphans) don't produce false positive waste
        -- charges on the primary.
        s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true,
        })
        s:create_index('pk', {
            run_count_per_level = 100,
        })
        s:create_index('sk', {
            parts = {2, 'unsigned'},
            unique = false,
            run_count_per_level = 100,
        })

        local pad = string.rep('x', 100)
        -- Run 1: keys 1..50 (sk = i).
        for i = 1, 50 do s:replace{i, i, pad} end
        box.snapshot()
        -- Run 2: keys 51..100 (sk = i).
        for i = 51, 100 do s:replace{i, i, pad} end
        box.snapshot()

        -- Non-overlapping PKs → every SK entry has a valid PK
        -- match → no orphans → no waste charged to the primary.
        -- SK range scans + SK point lookups.
        for _ = 1, 30 do
            s.index.sk:select()
            for i = 1, 100 do
                s.index.sk:select({i}, {limit = 1})
            end
        end

        t.assert_equals(s.index.pk:stat().disk.compaction.count, 0,
                        'no compaction in a no-garbage SK range')

        s:drop()
        box.cfg{vinyl_cache = old_cache}
    end)
end
