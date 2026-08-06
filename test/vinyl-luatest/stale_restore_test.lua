local server = require('luatest.server')
local t = require('luatest')
local g = t.group('stale_restore')

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new()
    cg.server:start()
end)
g.after_all(function(cg) cg.server:drop() end)
g.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        if box.space.test ~= nil then box.space.test:drop() end
    end)
end)

-- A reader below a committed insert skips it and must witness
-- the skip through a dump that rotates the source holding it:
-- the reader parks on a disk read after the skip, the skipped
-- statement moves from memory to a run, and the resumed walk
-- completes the chain. The witness must survive the rotation,
-- or the chain spans the key and a later reader at the latest
-- view is served the gap.
g.test_witness_survives_rotation = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{10, 6, 9}
        s:replace{30, 6, 9}
        s:replace{99, 1, 1}
        box.snapshot()
        -- The reader pins its view below the insert.
        local pinned = fiber.channel(1)
        local go = fiber.channel(1)
        local done = fiber.channel(1)
        local reader = fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            s:get{99}
            pinned:put(true)
            go:get()
            done:put(s.index.sk:select({6}, {iterator = 'GE'}))
            box.commit()
        end)
        reader:set_joinable(true)
        t.assert(pinned:get())
        s:replace{20, 6, 9}
        -- Park the reader's disk reads; let it skip {20} in
        -- memory and hang before yielding {30} from the run.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        go:put(true)
        fiber.sleep(0.1)
        -- Rotate: the dump moves {20} into a run while the
        -- reader is parked mid-scan.
        local dumper = fiber.create(function()
            box.snapshot()
        end)
        dumper:set_joinable(true)
        fiber.sleep(0.1)
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        t.assert_equals(done:get(), {{10, 6, 9}, {30, 6, 9}})
        reader:join()
        dumper:join()
        -- The latest view must see the inserted row.
        t.assert_equals(s.index.sk:select({6}, {iterator = 'GE'}),
                        {{10, 6, 9}, {20, 6, 9}, {30, 6, 9}})
    end)
end
