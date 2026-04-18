--
-- Benchmark: sensor (id, timestamp) tail-append workload.
--
-- The test populates ~300 MB of historical data across 30 sensors
-- in one range, appends a small tail to each sensor, and forces a
-- compaction. For the baseline (pre-page-copy) this rewrites the
-- whole range. For the page-copy implementation, cold pages are
-- expected to be reused verbatim, dropping compaction I/O, CPU,
-- and wall time.
--
-- Run manually (long-running):
--   ./test/test-run.py --vardir /tmp/t \
--     vinyl-luatest/compaction_stitch_bench_test.lua
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {
            checkpoint_interval = 0,
            vinyl_memory = 256 * 1024 * 1024,
            vinyl_cache = 0,
        },
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.test_sensor_tail_append = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local log = require('log')

        local SENSORS = 30
        local HISTORY = 10000
        local TAIL = 50
        local VALUE = string.rep('x', 1000)

        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {{1, 'unsigned'}, {2, 'unsigned'}},
            page_size = 16 * 1024,
        })

        local t0 = fiber.time()
        for sensor = 1, SENSORS do
            for ts = 1, HISTORY do
                s:replace{sensor, ts, VALUE}
            end
        end
        box.snapshot()
        local disk_mb = tonumber(s.index.pk:stat().disk.bytes) /
                        1024 / 1024
        log.info('sensor_bench: populate %d sensors x %d tuples ' ..
                 'in %.1fs, disk=%.1fMB',
                 SENSORS, HISTORY, fiber.time() - t0, disk_mb)

        local t1 = fiber.time()
        for sensor = 1, SENSORS do
            for ts = HISTORY + 1, HISTORY + TAIL do
                s:replace{sensor, ts, VALUE}
            end
        end
        box.snapshot()
        log.info('sensor_bench: tail-append %d x %d in %.1fs',
                 SENSORS, TAIL, fiber.time() - t1)

        local stat_before = s.index.pk:stat().disk.compaction
        local t2 = fiber.time()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 120, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count,
                        tonumber(stat_before.count))
        end)
        local dur = fiber.time() - t2
        local stat_after = s.index.pk:stat().disk.compaction

        local in_mb = (tonumber(stat_after.input.bytes) -
                       tonumber(stat_before.input.bytes)) / 1024 / 1024
        local out_mb = (tonumber(stat_after.output.bytes) -
                        tonumber(stat_before.output.bytes)) / 1024 / 1024
        log.info('sensor_bench: compact in=%.1fMB out=%.1fMB ' ..
                 'ratio=%.2f dur=%.1fs',
                 in_mb, out_mb, out_mb > 0 and in_mb / out_mb or 0, dur)

        t.assert_equals(s:len(), SENSORS * (HISTORY + TAIL))
        s:drop()
    end)
end
