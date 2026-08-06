local server = require('luatest.server')
local t = require('luatest')
local g = t.group('reinsert_link')

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new({
        box_cfg = {
            vinyl_page_size = 32,
            vinyl_defer_deletes = true,
        },
    })
    cg.server:start()
end)
g.after_all(function(cg) cg.server:drop() end)
g.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        if box.space.test ~= nil then box.space.test:drop() end
    end)
end)

-- A scan at the latest view proves a deferred-delete orphan
-- dead and parks on a disk read before reaching the end of its
-- key. A writer commits a fresh row into the not-yet-claimed
-- tail of the scanned range: the row's primary key is one the
-- scan never read, so the write conflicts with nothing and the
-- scan keeps its view. On resume the scan must not claim the
-- tail it slept through.
g.test_insert_under_pending_trailing_link = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local pad = string.rep('x', 100)
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        -- Two orphans-to-be on separate run pages: the second
        -- gives the scan a disk read to park on after it has
        -- resolved the first.
        s:replace{3, 4, 10, pad}
        s:replace{8, 4, 11, pad}
        box.snapshot()
        s:replace{5, 4, 4, pad}
        -- The overwrites orphan both secondary keys.
        s:replace{3, 9, 9, pad}
        s:replace{8, 9, 9, pad}
        local paused = fiber.channel(1)
        local go = fiber.channel(1)
        local reader = fiber.create(function()
            box.begin()
            local res = {}
            for _, tup in s.index.sk:pairs({4}) do
                table.insert(res, tup[1])
                if #res == 1 then
                    paused:put(true)
                    go:get()
                end
            end
            box.commit()
            paused:put(res)
        end)
        reader:set_joinable(true)
        -- The first step serves the live row and reads the run
        -- page holding the first orphan.
        t.assert(paused:get())
        -- The second step proves the first orphan dead and
        -- parks on the page holding the second.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        go:put(true)
        fiber.sleep(0.1)
        -- A fresh row lands behind the parked scan's position:
        -- the resumed walk may skip it, but must witness the
        -- skip. Its primary key is new to the scan, so the
        -- write conflicts with none of the scan's reads.
        s:replace{2, 4, 10, pad}
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        t.assert_equals(paused:get(), {5})
        reader:join()
        -- The new row is alive and must be served.
        t.assert_equals(s.index.sk:select{4},
                        {{5, 4, 4, pad}, {2, 4, 10, pad}})
    end)
end

-- A snapshot reader proves a deferred-delete orphan dead at its
-- view and records the verdict in the cache. The row was
-- re-created under the same secondary key above the reader's
-- view: the verdict holds at the forming view alone, and no
-- later write will sever it. If it is served above the
-- re-insert, the key is silently lost.
g.test_dead_claim_hides_reinserted_key = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local pad = string.rep('x', 100)
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{3, 4, 10, pad}
        s:replace{99, 1, 1, pad}
        -- The victim goes to a run: its deferred DELETE will
        -- never reach the secondary index in memory.
        box.snapshot()
        s:replace{5, 4, 4, pad}
        -- The overwrite orphans the victim's secondary key.
        s:replace{3, 9, 9, pad}
        -- The reader pins its view above the overwrite.
        box.begin({txn_isolation = 'snapshot'})
        s:get{99}
        -- The key is re-created above the reader's view.
        local w = fiber.create(function()
            s:replace{3, 4, 10, pad}
        end)
        w:set_joinable(true)
        t.assert_equals({w:join()}, {true})
        -- The reader proves the orphan dead at its view -- the
        -- newest primary version it sees is the overwrite --
        -- and caches the verdict.
        t.assert_equals(s.index.sk:select{4}, {{5, 4, 4, pad}})
        box.commit()
        -- Above the re-insert the key is alive again and the
        -- cached verdict must not be served.
        t.assert_equals(s.index.sk:select{4},
                        {{5, 4, 4, pad}, {3, 4, 10, pad}})
    end)
end
