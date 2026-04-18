local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    cg.server = server:new({
        box_cfg = {
            checkpoint_interval = 0,
            vinyl_memory = 256 * 1024 * 1024,
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
    end)
end)

--
-- Every interim write overlaps an existing key. Every page must
-- go through the merge path to resolve newer-vs-older; nothing
-- can be byte-copied. Baseline correctness.
--
g.test_compact_full_overlap = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 4000
        for i = 1, K do s:replace{i, 'old_' .. i} end
        box.snapshot()
        for i = 1, K do s:replace{i, 'new_' .. i} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        t.assert_equals(s:len(), K)
        for i = 1, K, math.floor(K / 20) do
            t.assert_equals(s:get{i}[2], 'new_' .. i)
        end
    end)
end

--
-- Interim writes form a small contiguous cluster inside the
-- larger existing range. Most source pages have no overlap with
-- the cluster; a subset does. The future copy-page optimization
-- will copy the non-overlapping pages and merge the overlapping
-- ones. The output must keep the older value outside the cluster
-- and the newer value inside.
--
g.test_compact_mixed_cluster = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 4000
        for i = 1, K do s:replace{i, 'old_' .. i} end
        box.snapshot()
        for i = 1000, 1200 do s:replace{i, 'new_' .. i} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        t.assert_equals(s:len(), K)
        for _, i in ipairs({1, 500, 999, 1201, 2000, 4000}) do
            t.assert_equals(s:get{i}[2], 'old_' .. i, 'cold key ' .. i)
        end
        for i = 1000, 1200 do
            t.assert_equals(s:get{i}[2], 'new_' .. i, 'hot key ' .. i)
        end
    end)
end

--
-- Deletes in the interim batch must cancel matching inserts from
-- the older run. If the implementation byte-copies a page that
-- the interim batch tombstones, the deleted key would survive.
--
g.test_compact_deletes = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 4000
        for i = 1, K do s:replace{i, i} end
        box.snapshot()
        for i = 1000, 1500 do s:delete{i} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        t.assert_equals(s:len(), K - 501)
        for i = 1000, 1500 do
            t.assert_equals(s:get{i}, nil, 'deleted key ' .. i)
        end
        for _, i in ipairs({1, 999, 1501, K}) do
            t.assert_not_equals(s:get{i}, nil, 'live key ' .. i)
        end
    end)
end

--
-- Sparse interim writes across the whole key range: every N-th
-- key gets overwritten. Many source pages have only one or two
-- overlapping keys; the merge must process them exactly, and the
-- large cold stretches between hot keys are the prime candidates
-- for page copy. Tests that no keys are lost or duplicated.
--
g.test_compact_sparse_overlap = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 8000
        for i = 1, K do s:replace{i, string.rep('a', 100)} end
        box.snapshot()
        for i = 1, K, 100 do s:replace{i, string.rep('b', 100)} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        t.assert_equals(s:len(), K)
        for _, i in ipairs({1, 100, 101, 4000, 4001, K}) do
            local expected = (i - 1) % 100 == 0
                and string.rep('b', 100) or string.rep('a', 100)
            t.assert_equals(s:get{i}[2], expected, 'key ' .. i)
        end
    end)
end

--
-- Small range_size splits the space into multiple ranges. Pages
-- that straddle a range boundary must be merged (to filter
-- out-of-range keys in each range's output), not byte-copied.
-- Neighboring pages that sit entirely inside one range may
-- still be copied. The final content must be the same as
-- without the split.
--
g.test_compact_with_range_split = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
            range_size = 1024 * 1024,
        })
        local K = 40000
        for i = 1, K do s:replace{i, string.rep('x', 200)} end
        box.snapshot()
        for i = 1, K, 20 do s:replace{i, string.rep('y', 200)} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        t.assert_equals(s:len(), K)
        for _, i in ipairs({1, 20, 21, 20000, 40000}) do
            local expected = (i - 1) % 20 == 0
                and string.rep('y', 200) or string.rep('x', 200)
            t.assert_equals(s:get{i}[2], expected, 'key ' .. i)
        end
    end)
end

--
-- Bloom filter correctness: after compaction, every present key
-- must still be found (no false negatives). A naive page-copy
-- that forgets to contribute hashes to the output bloom would
-- surface as missed lookups via iterators that trust the bloom.
--
g.test_compact_bloom_correctness = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
            bloom_fpr = 0.05,
        })
        local K = 4000
        for i = 1, K do s:replace{i, i} end
        box.snapshot()
        for i = 1, K, 50 do s:replace{i, i * 2} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        for i = 1, K, 13 do
            t.assert_not_equals(s:get{i}, nil, 'present key ' .. i)
        end
        for i = 1000000000, 1000000020 do
            t.assert_equals(s:get{i}, nil, 'absent key ' .. i)
        end
    end)
end

--
-- A sequence of compactions with different overlap patterns.
-- Guards against state leaking between compactions: per-run
-- counters, blooms, range metadata must all stay consistent
-- across repeated runs with mixed inputs.
--
g.test_compact_multi_phase = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 2000
        for i = 1, K do s:replace{i, 'v1_' .. i} end
        box.snapshot()

        for i = 500, 600 do s:replace{i, 'v2_' .. i} end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 1)
        end)

        for i = 1, K, 50 do s:replace{i, 'v3_' .. i} end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 2)
        end)

        for i = 1500, 1700 do s:delete{i} end
        box.snapshot()
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_ge(s.index.pk:stat().disk.compaction.count, 3)
        end)

        t.assert_equals(s:len(), K - 201)
        t.assert_equals(s:get{1}[2], 'v3_1')
        t.assert_equals(s:get{2}[2], 'v1_2')
        t.assert_equals(s:get{500}[2], 'v2_500')
        t.assert_equals(s:get{1500}, nil)
        t.assert_equals(s:get{K}[2], 'v1_' .. K)
    end)
end

--
-- Iteration order: a full scan and a bounded range scan over a
-- compacted space must return keys in order, without duplicates
-- or gaps. Copied pages whose output page-index entries are
-- mangled would show up here as skips or repeated tuples.
--
g.test_compact_iteration_order = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {1, 'unsigned'},
            page_size = 512,
        })
        local K = 4000
        for i = 1, K do s:replace{i, i * 2} end
        box.snapshot()
        for i = 2000, 2100 do s:replace{i, i * 3} end
        box.snapshot()

        local prev = tonumber(s.index.pk:stat().disk.compaction.count)
        s.index.pk:compact()
        t.helpers.retrying({timeout = 30, delay = 0.2}, function()
            t.assert_gt(s.index.pk:stat().disk.compaction.count, prev)
        end)

        local seen = 0
        local prev_key = 0
        for _, tup in s:pairs() do
            seen = seen + 1
            t.assert_gt(tup[1], prev_key, 'order at pos ' .. seen)
            prev_key = tup[1]
        end
        t.assert_equals(seen, K)

        local n = 0
        for _ in s:pairs({1500}, {iterator = 'GE'}) do
            n = n + 1
            if n >= 1000 then break end
        end
        t.assert_equals(n, 1000)
    end)
end
