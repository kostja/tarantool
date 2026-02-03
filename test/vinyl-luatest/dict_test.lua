local server = require('luatest.server')
local t = require('luatest')

-- Helpers registered as globals on the server via rawset(_G, ...).
-- luacheck: globals ROWS_PER_DUMP IDX_OPTS make_value fill wait_dump
-- luacheck: globals wait_compaction dump_until_dict

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({
        alias = 'master',
        env = {
            TARANTOOL_RUN_BEFORE_BOX_CFG = [[
local ffi = require('ffi')
ffi.cdef('void srand(unsigned int seed);')
ffi.C.srand(1)
math.randomseed(1)
            ]]
        },
        box_cfg = {
            checkpoint_interval = 0,
            checkpoint_count = 1,
        },
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

-- Register all helper functions as globals on the server.
-- Must be called before each test because server restarts
-- (in lifecycle/failure tests) clear the Lua state.
local function init_helpers(cg)
    cg.server:exec(function()
        rawset(_G, 'ROWS_PER_DUMP', 80)

        rawset(_G, 'IDX_OPTS', {
            parts = {1, 'unsigned'},
            compression_dict = true,
            page_size = 512,
            run_count_per_level = 10,
            run_size_ratio = 10,
            range_size = 1024 * 1024 * 1024,
        })

        rawset(_G, 'make_value', function(i, tag)
            local services = {"auth", "billing", "search", "profile",
                              "ads", "gateway", "storage", "feed"}
            local regions = {"eu-west", "us-east", "us-west",
                             "ap-south", "ru-central"}
            local levels = {"DEBUG", "INFO", "WARN", "ERROR"}
            local methods = {"GET", "POST", "PUT", "DELETE"}
            local paths = {"/v1/login", "/v1/pay", "/v1/items",
                           "/v1/profile", "/v1/feed", "/v1/search"}
            local chars = "abcdefghijklmnopqrstuvwxyz" ..
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"
            local chars_len = #chars
            local function rand_hex(n)
                local buf = {}
                for j = 1, n do
                    buf[j] = string.format("%x", math.random(0, 15))
                end
                return table.concat(buf)
            end
            local function rand_token(len)
                local buf = {}
                for j = 1, len do
                    local idx = math.random(1, chars_len)
                    buf[j] = chars:sub(idx, idx)
                end
                return table.concat(buf)
            end
            local function pick(a) return a[math.random(1, #a)] end
            return string.format(
                '{"tag":"%s","ts":"2026-02-10T18:%02d:%02dZ",' ..
                '"service":"%s","region":"%s","level":"%s",' ..
                '"http":{"method":"%s","path":"%s","status":%d},' ..
                '"trace":{"trace_id":"%s","span_id":"%s"},' ..
                '"user":{"id":"%s","session":"%s"},' ..
                '"tags":["tarantool","vinyl","dict"],' ..
                '"payload":"%s"}',
                tag, (i % 60), (i * 7) % 60,
                pick(services), pick(regions), pick(levels),
                pick(methods), pick(paths), 200 + (i % 5),
                rand_hex(20), rand_hex(10),
                rand_token(16), rand_token(24),
                rand_token(160 + (i % 20))
            )
        end)

        rawset(_G, 'fill', function(space, start, count, tag)
            for i = start, start + count - 1 do
                space:replace{i, make_value(i, tag)}
            end
        end)

        rawset(_G, 'wait_dump', function(pk, count)
            t.helpers.retrying({timeout = 30}, function()
                t.assert_ge(pk:stat().disk.dump.count, count)
            end)
        end)

        rawset(_G, 'wait_compaction', function(pk, count)
            t.helpers.retrying({timeout = 30}, function()
                t.assert_ge(pk:stat().disk.compaction.count, count)
            end)
        end)

        rawset(_G, 'dump_until_dict', function(space, pk, max_dumps)
            local created_before = box.stat.vinyl().dict.dicts_created
            for i = 1, max_dumps do
                -- Use the same key range every dump so that runs
                -- overlap and vy_compaction_plan_trim is a no-op.
                fill(space, 1, ROWS_PER_DUMP, 'dict' .. i)
                box.snapshot()
                wait_dump(pk, i)
                if box.stat.vinyl().dict.dicts_created >
                        created_before then
                    return i
                end
            end
            t.fail('dict not created after ' .. max_dumps .. ' dumps')
        end)

    end)
end

g.before_each(function(cg)
    init_helpers(cg)
end)

-- ----------------------------------------------------------------
-- Test: basic dictionary training, stats, data readability, and
-- that dropping a space with an active dict does not leak.
-- ----------------------------------------------------------------
g.test_dict_training_and_stats = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_training',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        local dumps = dump_until_dict(s, pk, 6)

        local stat = box.stat.vinyl().dict
        t.assert_gt(stat.train_attempted, 0, 'training attempted')
        t.assert_gt(stat.train_accepted, 0, 'training accepted')
        t.assert_gt(stat.dicts_created, 0, 'dicts created')
        t.assert_gt(stat.dicts_active, 0, 'dict is active')
        t.assert_gt(stat.dicts_bytes, 0,
                    'dicts_bytes positive after creation')
        t.assert_gt(stat.dicts_bytes_peak, 0,
                    'dicts_bytes_peak positive after creation')

        -- Per-index dict stats.
        local idx_stat = pk:stat().dict
        t.assert_gt(idx_stat.sample_size, 0,
                    'sample_size is set')
        t.assert_gt(idx_stat.dict_rate, 0,
                    'dict_rate is set')
        t.assert_gt(idx_stat.no_dict_rate, 0,
                    'no_dict_rate is set')

        t.assert_equals(s:count(), ROWS_PER_DUMP)

        -- Verify data content round-trip: dict-compressed data
        -- must decompress to the original values.
        local rows = s:select({}, {limit = 10})
        for _, row in ipairs(rows) do
            t.assert_type(row[2], 'string', 'value is a string')
            t.assert_gt(#row[2], 0, 'value is non-empty')
            t.assert_str_contains(row[2], '"tag":"dict',
                                  'JSON structure intact')
        end

        -- Verify that dropping the space releases the dict.
        local dropped_before = stat.dicts_dropped
        t.assert_gt(stat.dicts_active, 0, 'dict active before drop')

        s:drop()

        t.helpers.retrying({timeout = 30}, function()
            local st = box.stat.vinyl().dict
            t.assert_gt(st.dicts_dropped, dropped_before,
                        'dict dropped after space drop')
            t.assert_equals(st.dicts_bytes, 0,
                            'dicts_bytes zero after drop')
        end)
    end)
end

-- ----------------------------------------------------------------
-- Test: disabling compression_dict prevents new runs from using
-- an existing dictionary, and re-enabling resumes training.
-- ----------------------------------------------------------------
g.test_dict_disable_and_reenable = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_disable',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        dump_until_dict(s, pk, 6)
        local created_after_first = box.stat.vinyl().dict.dicts_created

        -- Disable: new runs must not use dict.
        pk:alter({compression_dict = false})
        local dump_count = pk:stat().disk.dump.count
        local created_before_disable = box.stat.vinyl().dict.dicts_created

        fill(s, 1, ROWS_PER_DUMP, 'nodict')
        box.snapshot()
        wait_dump(pk, dump_count + 1)

        -- No new dict should have been created while disabled.
        t.assert_equals(box.stat.vinyl().dict.dicts_created,
                        created_before_disable,
                        'no new dicts created while disabled')

        -- Re-enable: training must resume and create a new dict.
        pk:alter({compression_dict = true})
        dump_count = pk:stat().disk.dump.count
        local max_dumps = 12

        for i = 1, max_dumps do
            fill(s, 1, ROWS_PER_DUMP, 'on' .. i)
            box.snapshot()
            wait_dump(pk, dump_count + i)
            if box.stat.vinyl().dict.dicts_created >
                    created_before_disable then
                break
            end
        end

        t.assert_gt(box.stat.vinyl().dict.dicts_created,
                    created_after_first,
                    string.format('no new dict after re-enable '..
                                  '(%d dumps, throttled=%d, '..
                                  'attempted=%d)',
                                  max_dumps,
                                  box.stat.vinyl().dict.train_throttled,
                                  box.stat.vinyl().dict.train_attempted))

        -- At this point, the index has runs referencing the old
        -- dict (from the first training) and runs referencing the
        -- new dict (from the re-enable training). Compact
        -- everything to verify that merging runs with different
        -- dict IDs works correctly.
        local total = s:count()
        pk:compact()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.compaction.count, 1)
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- Data must survive compaction of mixed-dict runs.
        t.assert_equals(s:count(), total,
                        'row count preserved after mixed-dict compaction')
        local rows = s:select({}, {limit = 5})
        for _, row in ipairs(rows) do
            t.assert_type(row[2], 'string')
            t.assert_gt(#row[2], 0, 'value readable after compaction')
        end

        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: concurrent page read does not get "Dictionary mismatch"
-- when compaction removes the run the reader is accessing.
-- ----------------------------------------------------------------
g.test_dict_compaction_race = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local errinj = box.error.injection

        local s = box.schema.space.create('test_race',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        dump_until_dict(s, pk, 6)
        t.assert_gt(box.stat.vinyl().dict.dicts_active, 0,
                    'dict active before compaction race')

        local saved_cache = box.cfg.vinyl_cache
        box.cfg{vinyl_cache = 0}
        errinj.set('ERRINJ_VY_READ_PAGE_CB_SLEEP', true)

        local ch = fiber.channel(1)
        local reader = fiber.create(function()
            local ok, res = pcall(s.select, s)
            ch:put({ok, res})
        end)
        -- Wait until the reader fiber is blocked inside the
        -- error injection (its cbus message was sent and it is
        -- suspended waiting for the reply).
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(fiber.status(reader), 'suspended')
        end)

        -- Force compaction while the reader is suspended.
        -- Release the injection immediately after scheduling
        -- so that cbus delivery is not delayed.  The race is
        -- already set up: the reader holds refs to runs that
        -- compaction will remove from the range.
        pk:compact()
        errinj.set('ERRINJ_VY_READ_PAGE_CB_SLEEP', false)
        wait_compaction(pk, 1)

        local result = ch:get(5)
        t.assert(result[1], 'select succeeded without error')
        t.assert_equals(#result[2], ROWS_PER_DUMP)

        box.cfg{vinyl_cache = saved_cache}
        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: vylog write failure when preparing a run with a new dict.
-- The dict must be properly logged on the next successful dump,
-- and data must survive restart.
-- ----------------------------------------------------------------
g.test_dict_vylog_failure = function(cg)
    cg.server:exec(function()
        local errinj = box.error.injection
        local s = box.schema.space.create('test_vylog_fail',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        dump_until_dict(s, pk, 6)

        local dump_count = pk:stat().disk.dump.count
        local row_count = s:count()

        errinj.set('ERRINJ_VY_LOG_FLUSH', true)
        fill(s, row_count + 1, ROWS_PER_DUMP, 'fail')
        pcall(box.snapshot)
        errinj.set('ERRINJ_VY_LOG_FLUSH', false)

        fill(s, row_count + ROWS_PER_DUMP + 1, ROWS_PER_DUMP, 'ok')
        box.snapshot()
        wait_dump(pk, dump_count + 1)

        t.assert_equals(s:count(), row_count + 2 * ROWS_PER_DUMP)
    end)

    cg.server:restart()
    init_helpers(cg)

    cg.server:exec(function()
        local s = box.space.test_vylog_fail
        t.assert_gt(s:count(), 0, 'data recovered after vylog failure')
        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: dict lifecycle in vylog — dict data survives checkpoint
-- after the originating run is compacted, and is cleaned up when
-- all referencing runs are gone.
-- ----------------------------------------------------------------
g.test_dict_vylog_lifecycle = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_lifecycle',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        dump_until_dict(s, pk, 6)

        -- Compact to merge runs. The dict must stay active
        -- because the compaction output still references it.
        pk:compact()
        wait_compaction(pk, 1)

        t.assert_gt(box.stat.vinyl().dict.dicts_active, 0,
                    'dict active after compaction')

        -- Checkpoint to persist the current state to vylog.
        fill(s, 1, ROWS_PER_DUMP, 'gc')
        box.snapshot()
        t.assert_gt(s:count(), 0, 'data readable before restart')
    end)

    -- Restart: if the dict wasn't persisted in vylog, recovery
    -- would fail or data would be unreadable (runs reference
    -- the dict for decompression).
    cg.server:restart()
    init_helpers(cg)

    cg.server:exec(function()
        local s = box.space.test_lifecycle
        t.assert_gt(s:count(), 0, 'data recovered')
        t.assert_is_not(s:select({1}, {limit = 1})[1], nil)
        t.assert_gt(box.stat.vinyl().dict.dicts_active, 0,
                    'dict restored from vylog')

        -- Disable dict and compact everything into non-dict runs.
        local pk = s.index.pk
        pk:alter({compression_dict = false})
        local dump_count = pk:stat().disk.dump.count
        local row_count = s:count()

        fill(s, 1, ROWS_PER_DUMP, 'nodict')
        box.snapshot()
        wait_dump(pk, dump_count + 1)

        pk:compact()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.compaction.count, 1)
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        -- Snapshots trigger GC which forgets old dict-referencing
        -- runs.  Once all references are gone the dict is dropped.
        -- Note: within a single box.snapshot(), vylog rotation
        -- happens BEFORE GC, so the snapshot that drops the dict
        -- from memory still writes it to the vylog.  One more
        -- snapshot is needed to produce a clean vylog.
        t.helpers.retrying({timeout = 30}, function()
            box.snapshot()
            t.assert_equals(box.stat.vinyl().dict.dicts_active, 0)
        end)
        -- One more snapshot to flush the clean state to vylog.
        fill(s, 1, ROWS_PER_DUMP, 'flush')
        box.snapshot()
        t.assert_equals(box.stat.vinyl().dict.dicts_active, 0,
                        'dict dropped from memory')
        t.assert_gt(s:count(), 0, 'data still readable')
    end)

    -- Final restart: if the vylog still had stale dict records,
    -- recovery would recreate the dict, making dicts_active > 0.
    cg.server:restart()
    init_helpers(cg)

    cg.server:exec(function()
        local s = box.space.test_lifecycle
        t.assert_gt(s:count(), 0,
                    'data recovered after dict cleanup')
        t.assert_equals(box.stat.vinyl().dict.dicts_active, 0,
                        'no stale dict after recovery')
        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: two indexes on the same space with different dict settings
-- do not interfere with each other.
-- ----------------------------------------------------------------
g.test_dict_multi_index = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_multi_idx',
                                          {engine = 'vinyl'})
        -- Primary index with dict.
        local pk = s:create_index('pk', {
            parts = {1, 'unsigned'},
            compression_dict = true,
            page_size = 512,
            run_count_per_level = 10,
            run_size_ratio = 10,
            range_size = 1024 * 1024 * 1024,
        })
        -- Secondary index without dict.
        local sk = s:create_index('sk', {
            parts = {2, 'string'},
            compression_dict = false,
            page_size = 512,
            run_count_per_level = 10,
            run_size_ratio = 10,
            range_size = 1024 * 1024 * 1024,
            unique = false,
        })

        local created_before = box.stat.vinyl().dict.dicts_created
        dump_until_dict(s, pk, 6)

        -- PK should have a dict (dicts_created increased).
        t.assert_gt(box.stat.vinyl().dict.dicts_active, 0,
                    'pk has an active dict')

        -- Only one dict should have been created (for PK, not SK).
        t.assert_equals(box.stat.vinyl().dict.dicts_created,
                        created_before + 1,
                        'exactly one dict created (for pk only)')

        -- Verify data is readable through both indexes.
        -- If SK accidentally used a dict, reads would fail.
        t.assert_gt(s:count(), 0)
        t.assert_is_not(pk:select({1}, {limit = 1})[1], nil)
        t.assert_is_not(sk:select(
            {pk:select({1}, {limit = 1})[1][2]},
            {limit = 1})[1], nil, 'sk reads work')

        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: when a dump produces too few samples (< VY_DICT_MIN_SAMPLES),
-- dict training is skipped and no dict is created. Also verify that
-- non-training dumps increment train_throttled.
-- ----------------------------------------------------------------
g.test_dict_small_data_skip = function(cg)
    cg.server:exec(function()
        -- Use a large page_size so that a small amount of data
        -- fits into very few pages, below VY_DICT_MIN_SAMPLES.
        local s = box.schema.space.create('test_small',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', {
            parts = {1, 'unsigned'},
            compression_dict = true,
            -- Large pages → fewer pages → fewer samples.
            page_size = 64 * 1024,
            run_count_per_level = 10,
            run_size_ratio = 10,
            range_size = 1024 * 1024 * 1024,
        })

        local created_before = box.stat.vinyl().dict.dicts_created
        local throttled_before = box.stat.vinyl().dict.train_throttled

        -- Insert only a handful of rows (small enough to fit in
        -- 1-2 pages with page_size = 64KB).  Even after several
        -- dumps, training cannot succeed because there are too
        -- few samples.
        for i = 1, 4 do
            for j = 1, 5 do
                local k = (i - 1) * 5 + j
                s:replace{k, make_value(k, 'small')}
            end
            box.snapshot()
            wait_dump(pk, i)
        end

        -- No dict should have been created.
        t.assert_equals(box.stat.vinyl().dict.dicts_created,
                        created_before,
                        'no dict created with small data')

        -- Dumps that don't fall on the training period boundary
        -- should increment train_throttled.
        t.assert_gt(box.stat.vinyl().dict.train_throttled,
                    throttled_before,
                    'train_throttled incremented for non-training dumps')

        -- Verify data is still readable.
        t.assert_equals(s:count(), 20)

        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: vylog rotation with an incomplete originating run does not
-- produce duplicate PREPARE_RUN records.  Also verifies that dict
-- training works during compaction (not just dump).
--
-- Exercises the fix for the two-pass emission bug in
-- vy_log_append_lsm: an originating run that has dict_data but is
-- still incomplete (PREPARE_RUN written, CREATE_RUN not yet) was
-- emitted in both the first pass (dict_data) and second pass
-- (is_incomplete), producing a "Duplicate run id" error on the
-- next vylog rotation.
--
-- We trigger this by using run_count_per_level = 2 (so auto-
-- compaction fires during rapid dumps) with a single large range.
-- The concurrent compaction may create an originating run (with
-- dict_data) that hasn't been committed (CREATE_RUN) yet when the
-- next box.snapshot() rotates the vylog.
-- ----------------------------------------------------------------
g.test_dict_vylog_rotation_incomplete = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_rotation',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', {
            parts = {1, 'unsigned'},
            compression_dict = true,
            page_size = 512,
            run_count_per_level = 2,
            run_size_ratio = 2,
            range_size = 1024 * 1024 * 1024,
        })

        -- Rapid dumps trigger auto-compaction. Each box.snapshot()
        -- rotates the vylog while compaction tasks may be in flight.
        -- Before the fix, an incomplete originating run written by
        -- a concurrent compaction would appear twice in the rotated
        -- vylog.
        for i = 1, 12 do
            fill(s, 1, ROWS_PER_DUMP, 'rot' .. i)
            box.snapshot()
            wait_dump(pk, i)
        end

        -- Wait for all scheduler tasks to finish.
        t.helpers.retrying({timeout = 30}, function()
            local sched = box.stat.vinyl().scheduler
            t.assert_equals(sched.tasks_inprogress, 0)
        end)

        -- Compaction must have occurred (run_count_per_level = 2
        -- triggers it automatically). This also means dict training
        -- had a chance to run during compaction, not just dump.
        t.assert_gt(pk:stat().disk.compaction.count, 0,
                    'compaction occurred')

        -- One more snapshot to force vylog rotation after all
        -- tasks are done.  This was the point where the duplicate
        -- PREPARE_RUN error used to manifest.
        fill(s, 1, ROWS_PER_DUMP, 'rotation_trigger')
        box.snapshot()

        -- If we get here without "Duplicate run id" error, the
        -- fix works.  Verify data integrity.
        t.assert_equals(s:count(), ROWS_PER_DUMP)

        -- Verify a dict was created (by dump or compaction).
        t.assert_gt(box.stat.vinyl().dict.dicts_created, 0)

        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: altering compression_level on a non-empty index takes
-- effect on subsequent dumps.
-- ----------------------------------------------------------------
g.test_alter_compression_level = function(cg)
    cg.server:exec(function()
        -- Reseed so the result is independent of prior test
        -- execution order and random state consumption.
        math.randomseed(42)
        local s = box.schema.space.create('test_comp_level',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', {
            parts = {1, 'unsigned'},
            compression_level = 1,
            compression_dict = false,
            run_count_per_level = 10,
            run_size_ratio = 10,
            range_size = 1024 * 1024 * 1024,
        })

        -- Use data with enough entropy so that higher compression
        -- levels can actually find better matches. Trivially
        -- repetitive data (e.g. string.rep('a', N)) compresses
        -- identically at any level.
        local function make_row(i)
            return string.format(
                '{"id":%d,"path":"/api/v1/item/%d",' ..
                '"token":"%s","data":"%s"}',
                i, i % 50,
                make_value(i, 'lvl'),
                make_value(i + 1000, 'lvl'))
        end

        for i = 1, 200 do s:replace{i, make_row(i)} end
        local prev = pk:stat().disk.dump.output
        box.snapshot()
        wait_dump(pk, 1)
        local out1 = pk:stat().disk.dump.output
        local bytes1 = out1.bytes - prev.bytes
        local comp1 = out1.bytes_compressed - prev.bytes_compressed
        t.assert_gt(bytes1, 0)
        t.assert_gt(comp1, 0)

        pk:alter({compression_level = 19})
        for i = 201, 400 do s:replace{i, make_row(i)} end
        prev = pk:stat().disk.dump.output
        box.snapshot()
        wait_dump(pk, 2)
        local out2 = pk:stat().disk.dump.output
        local bytes2 = out2.bytes - prev.bytes
        local comp2 = out2.bytes_compressed - prev.bytes_compressed
        t.assert_gt(bytes2, 0)
        t.assert_gt(comp2, 0)

        local rate1 = comp1 / bytes1
        local rate2 = comp2 / bytes2
        t.assert(rate2 <= rate1,
                 string.format('higher compression_level produces '..
                               'better ratio: rate1=%f rate2=%f',
                               rate1, rate2))

        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: when the data changes so that no-dict compression is
-- better than dict compression, the active dictionary should be
-- dropped automatically.
--
-- Phase 1: train a dictionary on structured JSON data.
-- Phase 2: switch to high-entropy data (2KB random values).
--           The first retraining replaces the JSON dict with one
--           trained on the new data.  Subsequent retraining on
--           similar high-entropy data produces a nearly identical
--           dict that cannot beat the current one — the training
--           is rejected, and because dict_gain < VY_DICT_MIN_GAIN_PCT,
--           the active dictionary is dropped.
-- Phase 3: compact to remove all runs referencing the old dict
--           and verify dicts_active drops to zero.
-- ----------------------------------------------------------------
g.test_dict_drop_when_no_dict_better = function(cg)
    cg.server:exec(function()
        -- High-entropy data: each tuple has a 20KB random value.
        -- We use 100 rows per dump to fill the 2MB sample buffer
        -- (VY_DICT_SAMPLE_MAX), keeping the 64KB dict at ~3% of
        -- the sample.  This minimizes overfitting in the trial
        -- compression, ensuring that dict_gain reflects real
        -- benefit rather than training-data memorization.
        local digest = require('digest')
        local random_rows = ROWS_PER_DUMP
        local function fill_random(space, start, count)
            for j = start, start + count - 1 do
                space:replace{j, digest.urandom(20000)}
            end
        end

        local s = box.schema.space.create('test_drop',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', IDX_OPTS)

        -- Phase 1: structured data, train a dictionary.
        dump_until_dict(s, pk, 6)

        -- One more dump to commit the dict (id != 0).
        local dump_count = pk:stat().disk.dump.count
        fill(s, 1, ROWS_PER_DUMP, 'commit')
        box.snapshot()
        wait_dump(pk, dump_count + 1)

        t.assert_gt(box.stat.vinyl().dict.dicts_active, 0,
                    'dict is active after training')

        -- Phase 2: switch to high-entropy random data and dump
        -- until the training is rejected.  The very first
        -- retraining cycle may accept a new dict (optimised for
        -- the new data), but subsequent cycles will reject it
        -- because the newly trained dict is nearly identical to
        -- the one already in use.
        dump_count = pk:stat().disk.dump.count
        local rejected_before = box.stat.vinyl().dict.train_rejected
        local max_dumps = 20
        local dumps_done = 0

        while box.stat.vinyl().dict.train_rejected ==
                rejected_before do
            dumps_done = dumps_done + 1
            t.assert_le(dumps_done, max_dumps,
                        'training not rejected after ' ..
                        max_dumps .. ' random dumps')
            fill_random(s, 1, random_rows)
            box.snapshot()
            wait_dump(pk, dump_count + dumps_done)
        end

        t.assert_lt(pk:stat().dict.dict_gain * 100, 5,
                    'dict_gain confirms dict does not help')

        -- Phase 3: compact everything.  Since dict_last was
        -- cleared, the compaction output will not reference any
        -- dictionary.  Once old runs are removed, the dict's
        -- refcount drops to zero and dicts_active decreases.
        pk:compact()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.compaction.count, 1)
            t.assert_equals(
                box.stat.vinyl().scheduler.tasks_inprogress, 0)
        end)

        t.helpers.retrying({timeout = 30}, function()
            box.snapshot()
            t.assert_equals(box.stat.vinyl().dict.dicts_active, 0,
                            'dict dropped after no-dict proved better')
        end)

        t.assert_gt(s:count(), 0, 'data still readable')
        s:drop()
    end)
end

-- ----------------------------------------------------------------
-- Test: multiple compactions with dict references.
-- Verifies no crashes from dict refcount mismanagement.
--
-- Uses a separate test group with a fresh server because the
-- previous tests may have restarted the shared server, leaving
-- the vinyl scheduler in a state where explicit compact() does
-- not trigger compaction.
-- ----------------------------------------------------------------
local g_refcount = t.group('vinyl_dict_refcount')

g_refcount.before_all(function(cg)
    cg.server = server:new({
        alias = 'refcount',
        box_cfg = {
            checkpoint_interval = 0,
        },
    })
    cg.server:start()
end)

g_refcount.after_all(function(cg)
    cg.server:drop()
end)

g_refcount.test_dict_vylog_refcount = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test_refcount',
                                          {engine = 'vinyl'})
        local pk = s:create_index('pk', {
            parts = {1, 'unsigned'},
            compression_dict = true,
            run_count_per_level = 2,
            run_size_ratio = 4,
            range_size = 1024 * 1024 * 1024,
        })

        for i = 1, 11 do
            local c1 = (i % 2 == 0) and 'x' or 'a'
            local c2 = (i % 2 == 0) and 'y' or 'b'
            for j = 1, 1000 do
                s:replace{j, string.rep(c1, 100) .. tostring(j) ..
                             '_' .. tostring(i) ..
                             string.rep(c2, 100)}
            end
            box.snapshot()
            t.helpers.retrying({timeout = 30}, function()
                t.assert_ge(pk:stat().disk.dump.count, i)
            end)
        end

        t.helpers.retrying({timeout = 30}, function()
            t.assert_gt(box.stat.vinyl().dict.dicts_created, 0)
        end)

        pk:compact()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.compaction.count, 1)
        end)

        local dump_count = pk:stat().disk.dump.count
        for j = 1, 1000 do
            s:replace{j, string.rep('a', 100) ..
                      tostring(j) .. '_post1' ..
                      string.rep('b', 100)}
        end
        box.snapshot()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.dump.count, dump_count + 1)
        end)

        for j = 1, 1000 do
            s:replace{j, string.rep('x', 100) ..
                      tostring(j) .. '_post2' ..
                      string.rep('y', 100)}
        end
        box.snapshot()
        t.helpers.retrying({timeout = 30}, function()
            t.assert_ge(pk:stat().disk.dump.count, dump_count + 2)
        end)

        s:drop()
    end)
end
