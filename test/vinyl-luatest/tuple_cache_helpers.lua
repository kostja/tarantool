--
-- Holder-fiber helpers shared by the tuple cache tests. The
-- module is required inside server:exec() bodies, so it runs on
-- the server; its state lives in the server's package.loaded and
-- is shared between the exec blocks of one test.
--
local fiber = require('fiber')
local t = require('luatest')

local M = {}

-- Suspend a transaction that has run @read: once a key it has read
-- is overwritten, it is sent to a read view, which pins the tx
-- manager horizon below the write. Returns a handle whose
-- release() commits the holder.
function M.pin_read_view(read)
    local pinned = fiber.channel(1)
    local release = fiber.channel(1)
    local holder = fiber.create(function()
        box.begin()
        read()
        pinned:put(true)
        release:get()
        box.commit()
    end)
    holder:set_joinable(true)
    pinned:get()
    return {
        release = function()
            release:put(true)
            holder:join()
        end,
    }
end

-- Pin the horizon at the current LSN: a holder reads a scratch
-- key and an immediate overwrite displaces it into a read view.
-- Everything committed after this call stays above the horizon.
function M.pin_horizon()
    local scratch = box.space.rv_pin
    if scratch == nil then
        scratch = box.schema.create_space(
            'rv_pin', {engine = 'vinyl'})
        scratch:create_index('pk')
    end
    local key = scratch:len() + 1
    scratch:replace{key}
    local pin = M.pin_read_view(function()
        scratch:get{key}
    end)
    scratch:replace{key, 2}
    return pin
end

-- A reader iterating a scan one row per step inside an open
-- transaction: step() returns the next row, nil at the end of
-- the scan. The pause between steps is a real yield in the
-- middle of the scan.
function M.stepped_scan(index, key, opts)
    local req = fiber.channel(1)
    local res = fiber.channel(1)
    local holder = fiber.create(function()
        box.begin()
        local gen, param, state = index:pairs(key, opts)
        while req:get() do
            local tuple
            state, tuple = gen(param, state)
            res:put({tuple ~= nil and tuple:totable()})
        end
        box.commit()
    end)
    holder:set_joinable(true)
    return {
        step = function()
            req:put(true)
            local r = res:get()
            return r[1] or nil
        end,
        stop = function()
            req:put(false)
            holder:join()
        end,
    }
end

-- A reader suspended in an open transaction that can be probed:
-- each probe(fn) runs fn inside the reader's transaction and
-- returns its result.
function M.suspended_reader(read)
    local pinned = fiber.channel(1)
    local req = fiber.channel(1)
    local res = fiber.channel(1)
    local holder = fiber.create(function()
        box.begin()
        read()
        pinned:put(true)
        while true do
            local fn = req:get()
            if fn == false then
                break
            end
            res:put(fn())
        end
        box.commit()
    end)
    holder:set_joinable(true)
    pinned:get()
    return {
        probe = function(fn)
            req:put(fn)
            return res:get()
        end,
        stop = function()
            req:put(false)
            holder:join()
        end,
    }
end

-- Suspend a write in the WAL: it prepares, stays unconfirmed until
-- released, and every reader meanwhile sees it as prepared data.
-- The handle's commit() releases the WAL and returns true once
-- the write lands; fail() fails the WAL write instead, so the
-- write rolls back, and returns false. Debug-only: uses
-- box.error.injection.
function M.suspended_write(write)
    local stmts = box.stat.vinyl().tx.statements
    box.error.injection.set('ERRINJ_WAL_DELAY', true)
    local ch = fiber.channel(1)
    fiber.create(function()
        local ok = pcall(write)
        ch:put(ok)
    end)
    t.helpers.retrying({}, function()
        t.assert_gt(box.stat.vinyl().tx.statements, stmts)
    end)
    return {
        commit = function()
            box.error.injection.set('ERRINJ_WAL_DELAY',
                                    false)
            return ch:get()
        end,
        fail = function()
            box.error.injection.set('ERRINJ_WAL_WRITE_DISK',
                                    true)
            box.error.injection.set('ERRINJ_WAL_DELAY',
                                    false)
            local ok = ch:get()
            box.error.injection.set('ERRINJ_WAL_WRITE_DISK',
                                    false)
            return ok
        end,
    }
end

-- The fibers alive before the test: kill_test_fibers() tells the
-- test's own fibers from the server's service fibers by this
-- snapshot.
local base_fibers = {}

function M.snap_base_fibers()
    base_fibers = {}
    for id in pairs(fiber.info()) do
        base_fibers[id] = true
    end
end

-- Cancel every fiber the test spawned: a suspended holder unwinds
-- and rolls its transaction back.
function M.kill_test_fibers()
    local self_id = fiber.self():id()
    for id in pairs(fiber.info()) do
        if not base_fibers[id] and id ~= self_id then
            pcall(fiber.kill, id)
        end
    end
end

return M
