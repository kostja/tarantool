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
    -- both the cached and uncached conflict detection paths,
    -- unless forced via VINYL_STRESS_CACHE.
    math.randomseed(os.time())
    local cache = tonumber(os.getenv('VINYL_STRESS_CACHE'))
    if cache == nil then
        cache = math.random(2) == 1 and 1024 * 1024 or 0
    end
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
        local function envnum(name, default)
            return tonumber(os.getenv(name)) or default
        end
        local bank = BankTest.new({
            accounts = envnum('VINYL_STRESS_ACCOUNTS', 100),
            writers = envnum('VINYL_STRESS_WRITERS', 20),
            readers = envnum('VINYL_STRESS_READERS', 20),
            duration = envnum('VINYL_STRESS_TEST_TIME', 2),
            ddl_nemesis = envnum('VINYL_STRESS_DDL', 1) ~= 0,
            wal_chaos = envnum('VINYL_STRESS_WAL_CHAOS', 1) ~= 0,
            snapshots = envnum('VINYL_STRESS_SNAPSHOTS', 1) ~= 0,
            pk_readers = tonumber(os.getenv('VINYL_STRESS_PK_READERS')),
            sk_readers = tonumber(os.getenv('VINYL_STRESS_SK_READERS')),
            mixed_readers =
                tonumber(os.getenv('VINYL_STRESS_MIXED_READERS')),
        })
        bank:run()
        bank:verify()
    end)
end
