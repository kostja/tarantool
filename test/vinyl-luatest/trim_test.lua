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
