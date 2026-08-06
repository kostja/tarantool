local server = require('luatest.server')
local t = require('luatest')
local g = t.group('stalefuse')

g.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
end)
g.after_all(function(cg) cg.server:drop() end)
g.after_each(function(cg)
    cg.server:exec(function()
        for _, name in ipairs({'test', 'churn'}) do
            if box.space[name] ~= nil then box.space[name]:drop() end
        end
    end)
end)

-- An update that leaves the indexed fields unchanged removes the
-- primary cache entry alone: the secondary entry becomes maybe
-- stale, invisible to readers but still an interior joint of its
-- chain. When the eviction hand collects it, the entry must not
-- fuse its links into one: the key still holds a live row, and a
-- fused link would claim the gap for every reader -- a scan
-- served the chain would skip the row.
g.test_collected_stale_entry_severs_chain = function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_cache = 0}
        box.cfg{vinyl_cache = 40 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{1, 9, 9}
        s:replace{2, 9, 9}
        s:replace{3, 9, 9}
        -- Warm the chain: the three rows link up.
        t.assert_equals(s.index.sk:select{9},
                        {{1, 9, 9}, {2, 9, 9}, {3, 9, 9}})
        -- The unindexed update: the middle row's primary entry
        -- goes, its secondary entry is maybe stale from now on.
        s:replace{2, 9, 9, 'x'}
        -- Churn until the hand has collected the stale entry.
        local churn = box.schema.space.create('churn',
                                              {engine = 'vinyl'})
        churn:create_index('pk')
        local pad = string.rep('p', 100)
        local i = 0
        t.helpers.retrying({timeout = 60}, function()
            for _ = 1, 50 do
                i = i + 1
                churn:replace{i, pad}
                churn:get(i)
            end
            t.assert_gt(s.index.sk:stat().cache.evict.rows, 0,
                'the stale entry must be collected')
        end)
        -- The middle row is alive: the scan must return it,
        -- whatever the collection left of the chain.
        t.assert_equals(s.index.sk:select{9},
                        {{1, 9, 9}, {2, 9, 9, 'x'}, {3, 9, 9}})
    end)
end
