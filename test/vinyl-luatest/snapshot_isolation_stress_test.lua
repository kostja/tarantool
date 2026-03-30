--
-- Snapshot isolation correctness stress test.
--
-- Uses BankTest (see bank_test.lua): N accounts with a fixed
-- total balance and unique tokens. Writers transfer money and
-- swap tokens. Readers verify both invariants.
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group()

g.before_all(function(cg)
    -- Randomly enable or disable the tuple cache to exercise
    -- both the cached and uncached conflict detection paths.
    math.randomseed(os.time())
    local cache = math.random(2) == 1 and 1024 * 1024 or 0
    cg.server = server:new({
        box_cfg = {
            txn_isolation = 'snapshot',
            vinyl_memory = 1024 * 1024, -- 1MB for faster dumps
            vinyl_cache = cache,
        },
    })
    cg.server:start()
end)

g.after_all(function(cg)
    cg.server:drop()
end)

g.after_each(function(cg)
    cg.server:exec(function()
        for _, sp in pairs(box.space) do
            if sp.id > box.schema.SYSTEM_ID_MAX then
                sp:drop()
            end
        end
    end)
end)

g.test_bank_account_invariant = function(cg)
    cg.server:exec(function()
        local BankTest = require('test.vinyl-luatest.bank_test')
        local bank = BankTest.new({
            accounts = 100,
            writers = 20,
            readers = 20,
            duration = 2,
        })
        bank:run()
        bank:verify()
    end)
end
