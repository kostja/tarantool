local server = require('luatest.server')
local t = require('luatest')

--
-- Tests for the v2 page index features: backup of .index2 files,
-- bloom filters with collation, concurrent DDL integrity, and
-- page bounds for small single-block runs.
--

-- Shared group lifecycle: drop server after all tests, drop
-- space.test after each test.
local function setup_group(group)
    group.after_all(function(cg)
        cg.server:drop()
    end)
    group.after_each(function(cg)
        cg.server:exec(function()
            if box.space.test ~= nil then
                box.space.test:drop()
            end
        end)
    end)
end

--
-- Helper for concurrent secondary index build tests.
-- Creates a space, populates it, then builds a secondary index
-- (with the given name and options) while a background fiber
-- concurrently inserts/updates/deletes random keys.  After the
-- build, asserts that primary and secondary index counts match.
--
local function test_index_build_impl(cg, index_name, index_opts)
    cg.server:exec(function(index_name, index_opts)
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')

        local last_val = 0
        for i = 1, 500 do
            last_val = last_val + 1
            s:replace{i, i, last_val}
        end
        box.snapshot()

        local ch = fiber.channel(1)
        fiber.create(function()
            for _ = 1, 200 do
                local op = math.random(4)
                local key = math.random(1000)
                local val1 = math.random(1000)
                last_val = last_val + 1
                local val2 = last_val
                if op == 1 then
                    pcall(s.insert, s, {key, val1, val2})
                elseif op == 2 then
                    pcall(s.replace, s, {key, val1, val2})
                elseif op == 3 then
                    pcall(s.delete, s, key)
                else
                    pcall(s.update, s, key,
                          {{'=', 2, val1}, {'=', 3, val2}})
                end
            end
            ch:put(true)
        end)
        s:create_index(index_name, index_opts)
        ch:get(10)

        local pk_count = s.index.pk:count()
        local sk_count = s.index[index_name]:count()
        t.assert_equals(pk_count, sk_count,
            string.format('pk:count()=%d must equal %s:count()=%d; ' ..
                          'bloom false negative during index build',
                          pk_count, index_name, sk_count))
    end, {index_name, index_opts})
end

--
-- Group: tests using a default server configuration.
-- Covers backup of .index2 files and concurrent secondary index
-- build integrity.
--
local g = t.group('default')

g.before_all(function(cg)
    cg.server = server:new({alias = 'master'})
    cg.server:start()
end)

setup_group(g)

--
-- Verify that box.backup.start() includes .index2 files (v2 page
-- index) in the file list, and that a server restored from the
-- backup has working vinyl indexes with functional bloom filters.
--
-- Before the fix, the backup test's do_backup() function did not
-- recognize the .index2 suffix, causing an assertion failure.
-- Even if the copy succeeded, a missing .index2 file would force
-- the restored server to rebuild page indexes from scratch, which
-- is correct but should be verified.
--
g.test_backup_includes_index2_files = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        -- Insert enough data to produce at least one .run and
        -- one .index2 file.
        for i = 1, 100 do
            s:replace{i, string.rep('x', 100)}
        end
        box.snapshot()

        local files = box.backup.start()
        -- Check that .index2 files are present in the backup list.
        local has_index2 = false
        for _, path in ipairs(files) do
            if path:match('%.index2$') then
                has_index2 = true
                break
            end
        end
        box.backup.stop()
        t.assert(has_index2,
            'backup file list must include .index2 files')
    end)
end

--
-- End-to-end test: backup a vinyl database, restore it to a new
-- server, and verify that point lookups work through the bloom
-- filter (i.e., .index2 files were correctly copied and loaded).
--
g.test_backup_restore_bloom_filter_works = function(cg)
    -- Phase 1: populate the master server.
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 100 do
            s:replace{i, 'value_' .. tostring(i)}
        end
        box.snapshot()
    end)

    -- Phase 2: take a backup.
    local backup_dir = cg.server:exec(function()
        local fio = require('fio')
        local files = box.backup.start()
        local backup_dir = fio.pathjoin(fio.cwd(), 'backup')
        for _, path in ipairs(files) do
            local suffix = path:match('%.([^.]+)$')
            local dir
            if suffix == 'xlog' then
                dir = box.cfg.wal_dir
            elseif suffix == 'snap' then
                dir = box.cfg.memtx_dir
            elseif suffix == 'vylog' or suffix == 'run' or
                   suffix == 'index' or suffix == 'index2' then
                dir = box.cfg.vinyl_dir
            end
            assert(dir ~= nil, 'unknown suffix: ' .. suffix)
            local rel_path = path:sub(#dir + 2)
            local dest_dir = fio.pathjoin(backup_dir,
                                          fio.dirname(rel_path))
            fio.mktree(dest_dir)
            fio.copyfile(path, fio.pathjoin(dest_dir,
                                            fio.basename(path)))
        end
        box.backup.stop()
        return backup_dir
    end)

    -- Phase 3: start a new server from the backup directory.
    local restore = server:new({
        alias = 'restore',
        datadir = backup_dir,
    })
    restore:start()

    -- Phase 4: verify data integrity and bloom filter function.
    restore:exec(function()
        local s = box.space.test
        t.assert(s ~= nil, 'space must exist after restore')

        -- Point lookups exercise the bloom filter.
        t.assert_equals(s:get(1), {1, 'value_1'})
        t.assert_equals(s:get(50), {50, 'value_50'})
        t.assert_equals(s:get(100), {100, 'value_100'})

        -- Verify bloom filter statistics: the 3 existing-key
        -- lookups must not produce false negatives (bloom_hit
        -- means "definitely absent" -- must be 0 for keys that
        -- exist).
        local bloom = s.index.pk:stat().disk.iterator.bloom
        t.assert_equals(bloom.hit, 0,
            'bloom filter must not report false negatives ' ..
            'for restored data')

        -- Non-existent key (bloom may legitimately report
        -- "absent" here -- that's a true negative).
        t.assert_equals(s:get(101), nil)

        -- Full scan to verify all data is intact.
        t.assert_equals(s:count(), 100)
    end)

    restore:drop()

    -- Cleanup.
    cg.server:exec(function(backup_dir)
        local fio = require('fio')
        fio.rmtree(backup_dir)
    end, {backup_dir})
end

--
-- Regression test for fuse8 bloom filter false negatives during
-- concurrent secondary index build.
--
-- When building a unique secondary index, vinyl reads existing
-- tuples to detect duplicates.  If the bloom filter incorrectly
-- reports that a key is absent (false negative), the build skips
-- the duplicate check for that key, allowing the secondary index
-- to silently lose entries.  This manifests as a count mismatch
-- between the primary and secondary indexes.
--
-- Adapted from engine/ddl.test.lua:check_fiber().
--
g.test_secondary_index_build_integrity = function(cg)
    test_index_build_impl(cg, 'sk', {
        unique = false,
        parts = {2, 'unsigned'},
    })
end

--
-- Same test but with a unique secondary index.  Uses auto-
-- incrementing field 3 to avoid legitimate duplicate errors
-- during index creation.
--
g.test_unique_secondary_index_build_integrity = function(cg)
    test_index_build_impl(cg, 'tk', {
        unique = true,
        parts = {3, 'unsigned'},
    })
end

--
-- Group: tests requiring vinyl_cache = 0 to force all lookups
-- through the bloom filter.  Covers collation-aware hashing.
--
local g_collation = t.group('collation')

g_collation.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {
            vinyl_cache = 0,
        },
    })
    cg.server:start()
end)

setup_group(g_collation)

-- Regression test for a bug where vy_stmt_hash64() did not apply
-- collation when hashing string fields for the fuse8 bloom filter.
-- As a result, keys that are equal under a case-insensitive collation
-- (e.g., 'Эль' and 'ЭЛЬ') would hash to different values, causing
-- false negatives: the filter would incorrectly report that a key is
-- absent, and point lookups (get/update/delete) would miss existing
-- rows.
g_collation.test_bloom_filter_respects_collation = function(cg)
    cg.server:exec(function()
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {{1, 'string', collation = 'unicode_ci'}},
        })
        -- Insert rows with mixed-case Cyrillic keys.
        s:insert{'Эль'}
        s:insert{'Ёж'}
        s:insert{'Юла'}
        -- Flush to disk so that subsequent lookups go through the
        -- bloom filter.
        box.snapshot()

        -- Point lookup with a different-case key that must match
        -- via unicode_ci collation.
        t.assert_equals(s:get('эль'), {'Эль'})
        t.assert_equals(s:get('ЭЛЬ'), {'Эль'})
        t.assert_equals(s:get('ёж'), {'Ёж'})
        t.assert_equals(s:get('ЁЖ'), {'Ёж'})
        t.assert_equals(s:get('юла'), {'Юла'})
        t.assert_equals(s:get('ЮЛА'), {'Юла'})

        -- Verify the bloom filter was consulted and did not produce
        -- false negatives (bloom_hit means the filter said "absent"
        -- and the lookup was skipped -- that must not happen here).
        local stat = s.index.pk:stat().disk.iterator.bloom
        t.assert_equals(stat.hit, 0,
            'bloom filter must not report false negatives for ' ..
            'collation-equivalent keys')

        -- Update via a different-case key.
        s:update('ЭЛЬ', {{'!', 2, 456}})
        t.assert_equals(s:get('Эль'), {'Эль', 456})

        -- Delete via a different-case key.
        s:delete('ёж')
        t.assert_equals(s:get('Ёж'), nil)
    end)
end

-- Test that the bloom filter works correctly with a composite key
-- where one part has a collation and another does not.
g_collation.test_bloom_filter_composite_key_with_collation = function(cg)
    cg.server:exec(function()
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {
                {1, 'string', collation = 'unicode_ci'},
                {2, 'unsigned'},
            },
        })
        s:insert{'Hello', 1}
        s:insert{'Hello', 2}
        s:insert{'World', 1}
        box.snapshot()

        -- Lookup with different case on the collated part.
        t.assert_equals(s:get{'HELLO', 1}, {'Hello', 1})
        t.assert_equals(s:get{'hello', 2}, {'Hello', 2})
        t.assert_equals(s:get{'WORLD', 1}, {'World', 1})

        -- Verify no false negatives for existing keys.  A bloom
        -- hit means "definitely absent" -- that must not happen for
        -- keys that actually exist.
        local stat = s.index.pk:stat().disk.iterator.bloom
        t.assert_equals(stat.hit, 0,
            'bloom filter must not report false negatives for ' ..
            'existing keys with collation')

        -- Non-existent key: the bloom filter may legitimately say
        -- "absent" here (true negative).
        t.assert_equals(s:get{'World', 2}, nil)
    end)
end

-- Regression test for vy_stmt_hash64() producing wrong hashes for
-- FIELD_TYPE_DOUBLE keys.  The re-encoding buffer was declared inside
-- an if-block; after the block ended the buffer went out of scope and
-- the compiler could clobber its stack memory, so PMurHash32_Process
-- hashed garbage.  Symptom: every DOUBLE lookup was a bloom false
-- negative, making selects return empty results.
g_collation.test_bloom_filter_double_key = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'double'}}})
        s:replace{-1.0, 'a'}
        s:replace{0.0, 'b'}
        s:replace{1.0, 'c'}
        box.snapshot()
        -- Evict the index cache so lookups hit the bloom filter.
        local saved = box.cfg.vinyl_index_cache
        box.cfg{vinyl_index_cache = 0}
        box.cfg{vinyl_index_cache = saved}

        for _, v in ipairs({-1.0, 0.0, 1.0}) do
            t.assert_equals(#s:select(v), 1,
                'bloom false negative for DOUBLE key ' .. tostring(v))
        end
        local stat = s.index.pk:stat().disk.iterator.bloom
        t.assert_equals(stat.hit, 0,
            'bloom filter must not report false negatives for DOUBLE keys')
    end)
end

-- Test that upsert works correctly with collation after flush.
g_collation.test_upsert_with_collation_after_flush = function(cg)
    cg.server:exec(function()
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {{1, 'string', collation = 'unicode_ci'}},
        })
        s:insert{'Test', 100}
        box.snapshot()

        -- Upsert with a different-case key should update the
        -- existing row, not insert a new one.
        s:upsert({'TEST', 200}, {{'+', 2, 50}})
        box.snapshot()

        local result = s:select({}, {fullscan = true})
        t.assert_equals(#result, 1, 'upsert must not create a duplicate')
        t.assert_equals(result[1], {'Test', 150})
    end)
end

--
-- Group: tests requiring a deterministic random seed for
-- reproducible compaction and split decisions.
--
local g_seeded = t.group('seeded')

g_seeded.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {
            log_level = 'verbose',
        },
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
    -- Register a server-side helper: write two overlapping dumps
    -- of keys 1..key_count with pad_size-byte values, producing
    -- two runs that share the same key range.
    cg.server:exec(function()
        rawset(_G, 'two_overlapping_dumps', function(s, key_count,
                                                     pad_size)
            for i = 1, key_count do
                s:replace{i, string.rep('x', pad_size)}
            end
            box.snapshot()
            for i = 1, key_count do
                s:replace{i, string.rep('y', pad_size)}
            end
            box.snapshot()
        end)
    end)
end)

setup_group(g_seeded)

g_seeded.test_empty_range_coalesced = function(cg)
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
        _G.two_overlapping_dumps(s, 40, 200)
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
-- Test that vinyl correctly computes page bounds for slices in
-- small runs where block_count == 1 (fewer than 64 pages).
--
-- With the v2 page index, page info is stored in a two-level
-- structure: the block directory (always in memory, one entry per
-- 64 pages) and demand-loaded index blocks (in the index cache).
-- When a run has fewer than 64 pages, block_count == 1 and all
-- slices map to the same single block.
--
-- Before the fix, vy_range_init_slice used only block-aligned
-- bounds, which made every slice in a single-block run appear to
-- span the entire run.  This broke:
--   - Byte estimates (every slice reported the full run size)
--   - Split decisions (vy_range_needs_split couldn't find a
--     median key different from the first key)
--   - Bloat detection (run_file_size / total_slice_bytes was
--     always 1.0, never exceeding the bloat threshold)
--
-- The fix: vy_range_init_slice uses the two-level page index
-- (with block-aligned fallback on I/O error), and
-- vy_range_needs_split falls back to the index cache for
-- page-level precision when mid_block == first_block.
--

--
-- Test that a small single-block run can still be split correctly.
--
-- Two overlapping dumps produce a merged run after compaction.
-- If the merged run exceeds range_size, a split should occur
-- even though the run has only a single block in its page index.
--
g_seeded.test_small_run_split = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- page_size=512, range_size=4096: 40 rows x 200 bytes
        -- yields ~8 KB per dump -> ~16 pages per dump.
        -- block_count will be 1 (< 64 pages per block).
        -- Two overlapping dumps -> compact -> merged run ~8 KB,
        -- exceeding range_size=4096 -> split.
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 512,
            range_size = 4096,
        })

        _G.two_overlapping_dumps(s, 40, 200)

        -- Force compaction to merge the 2 runs.
        s.index.pk:compact()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        -- The merged run (~8 KB) exceeds range_size (4 KB),
        -- so a split should occur.
        local stat = s.index.pk:stat()
        t.assert_ge(stat.range_count, 2,
            'small single-block runs must still split correctly')

        -- Verify data integrity after split.
        t.assert_equals(s:count(), 40)
        t.assert_equals(s:get(1)[1], 1)
        t.assert_equals(s:get(40)[1], 40)
    end)
end

--
-- Test that byte estimates for slices in single-block runs are
-- reasonable (not the full run size).
--
-- When block_count == 1 and vy_range_init_slice fell back to
-- block-aligned bounds, every slice would claim to span all pages
-- of the run, making the estimated size equal to the full run
-- size.  After a split, two ranges sharing the same run would
-- each claim the full run size, doubling the apparent disk usage.
--
g_seeded.test_small_run_byte_estimates = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- Use large range_size to prevent auto-split, so we
        -- can inspect byte estimates for a single range with
        -- two run files from disjoint dumps.
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 512,
            range_size = 1024 * 1024,
        })

        -- Two disjoint dumps (not overlapping, unlike the helper).
        for i = 1, 20 do
            s:replace{i, string.rep('x', 200)}
        end
        box.snapshot()
        for i = 21, 40 do
            s:replace{i, string.rep('y', 200)}
        end
        box.snapshot()

        local stat = s.index.pk:stat()
        t.assert_equals(stat.run_count, 2)
        t.assert_equals(stat.range_count, 1)

        -- The total disk bytes reported should be close to the
        -- actual data size (~8 KB for 40 rows x 200 bytes),
        -- not doubled due to slice overestimation.
        -- Allow a generous margin (3x) since page overhead and
        -- index data inflate the numbers, but it should definitely
        -- be less than if both slices claimed full-run extents
        -- (which would be ~16 KB before overhead).
        local actual_data = 40 * (200 + 20) -- payload + key/header
        t.assert_le(stat.disk.bytes, actual_data * 3,
            'slice byte estimates must not grossly overcount ' ..
            'for single-block runs')
    end)
end

--
-- Test that bloat detection works for single-block runs.
--
-- After a split, two ranges share one run file.  When one range
-- compacts its slice, the other range's slice pins unreferenced
-- data (bloat).  With correct page bounds, the bloat ratio
-- exceeds the threshold and triggers a bloat compaction.
--
-- With broken block-aligned bounds (all slices span the full
-- run), the bloat ratio is always 1.0 and bloat is never detected.
--
g_seeded.test_small_run_bloat_detection = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- 40 rows x 200 bytes ~ 8 KB per dump.  range_size=4096
        -- ensures a split after compaction.
        s:create_index('pk', {
            run_count_per_level = 100,
            page_size = 512,
            range_size = 4096,
        })

        -- Phase 1: two overlapping dumps -> compact -> split.
        _G.two_overlapping_dumps(s, 40, 200)
        s.index.pk:compact()

        t.helpers.retrying({timeout = 5}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
            t.assert_ge(s.index.pk:stat().range_count, 2)
        end)

        -- Phase 2: small dump overlapping one range, then compact.
        -- This consumes one range's slice of the shared run.
        -- The other range should detect bloat and compact too.
        local compaction_before =
            s.index.pk:stat().disk.compaction.count
        for i = 1, 5 do s:replace{i, string.rep('c', 200)} end
        box.snapshot()
        s.index.pk:compact()

        -- Wait for at least 2 compactions: one for the target
        -- range, and one bloat compaction for the other range.
        t.helpers.retrying({timeout = 10}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count,
                        compaction_before + 2,
                        'bloat compaction must trigger for ' ..
                        'the range sharing the old run file')
        end)

        -- Verify data integrity.
        t.assert_equals(s:count(), 40)
    end)
end
