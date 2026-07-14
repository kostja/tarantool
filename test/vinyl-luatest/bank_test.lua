--
-- Bank account workload for snapshot isolation stress testing.
--
-- N accounts with a fixed total balance. Writer TXs transfer
-- money between random accounts and swap unique tokens.
-- Reader TXs verify that the total is constant and tokens are
-- unique. Under snapshot isolation, readers must always see a
-- consistent snapshot.
--
local fiber = require('fiber')
local log = require('log')
local t = require('luatest')

local BankTest = {}
BankTest.__index = BankTest

function BankTest.new(params)
    local self = setmetatable({}, BankTest)
    self.accounts = params.accounts or 100
    self.initial_balance = params.initial_balance or 1000
    self.writers = params.writers or 20
    self.readers = params.readers or 20
    self.duration = params.duration or 5
    self.ddl_nemesis = params.ddl_nemesis ~= false
    self.wal_chaos = params.wal_chaos ~= false
    self.snapshots = params.snapshots ~= false
    self.pk_readers = params.pk_readers
    self.sk_readers = params.sk_readers
    self.mixed_readers = params.mixed_readers

    self.s = box.schema.space.create('account', {engine = 'vinyl'})
    self.s:create_index('pk')
    self.s:create_index('token', {parts = {3, 'unsigned'},
                                  unique = true})
    for i = 1, self.accounts do
        self.s:replace{i, self.initial_balance, i}
    end
    box.snapshot()

    self.expected_total = self.accounts * self.initial_balance
    self.stop = false
    self.fibers = {}
    self.reader_errors = {}
    self.write_count = 0
    self.read_count = 0
    self.conflict_count = 0
    return self
end

function BankTest:start_writer()
    local id = #self.fibers + 1
    local my_token = self.accounts + id
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            local from = math.random(1, self_.accounts)
            local to = math.random(1, self_.accounts)
            if from ~= to then
                local ok = pcall(function()
                    box.begin()
                    local f = self_.s:get(from)
                    local t_ = self_.s:get(to)
                    local amount = math.random(1, 10)
                    local old_token = my_token
                    self_.s:replace{from, f[2] - amount,
                                    old_token}
                    self_.s:replace{to, t_[2] + amount, f[3]}
                    box.commit()
                    my_token = t_[3]
                end)
                if ok then
                    self_.write_count = self_.write_count + 1
                else
                    self_.conflict_count = self_.conflict_count + 1
                    pcall(box.rollback)
                end
            end
            fiber.yield()
        end
    end))
end

function BankTest:start_reader()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            local ok = pcall(function()
                box.begin()
                local total = 0
                local tokens = {}
                for _, tuple in self_.s:pairs() do
                    total = total + tuple[2]
                    tokens[tuple[3]] = true
                end
                if total ~= self_.expected_total then
                    table.insert(self_.reader_errors,
                        string.format('expected total %d, got %d',
                                      self_.expected_total, total))
                end
                local token_count = 0
                for _ in pairs(tokens) do
                    token_count = token_count + 1
                end
                if token_count ~= self_.accounts then
                    table.insert(self_.reader_errors,
                        string.format(
                            'expected %d unique tokens, got %d',
                            self_.accounts, token_count))
                end
                self_.read_count = self_.read_count + 1
                box.commit()
            end)
            if not ok then
                pcall(box.rollback)
            end
            fiber.yield()
        end
    end))
end

-- Reader that scans through the token SK index and verifies
-- invariants via cross-referenced PK lookups. Exercises SK
-- cache population, PK point lookup cache, and mixed access.
function BankTest:start_sk_reader()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            local ok = pcall(function()
                box.begin()
                local total = 0
                local tokens = {}
                local pk_total = 0
                -- Scan via token SK index.
                for _, tuple in self_.s.index.token:pairs() do
                    total = total + tuple[2]
                    tokens[tuple[3]] = true
                    -- Cross-reference: point read via PK.
                    local pk_tuple = self_.s:get(tuple[1])
                    if pk_tuple == nil then
                        table.insert(self_.reader_errors,
                            string.format(
                                'SK entry pk=%d not in PK',
                                tuple[1]))
                    else
                        pk_total = pk_total + pk_tuple[2]
                    end
                end
                if total ~= self_.expected_total then
                    table.insert(self_.reader_errors,
                        string.format(
                            'SK scan: expected total %d, got %d',
                            self_.expected_total, total))
                end
                if pk_total ~= self_.expected_total then
                    table.insert(self_.reader_errors,
                        string.format(
                            'PK xref: expected total %d, got %d',
                            self_.expected_total, pk_total))
                end
                local token_count = 0
                for _ in pairs(tokens) do
                    token_count = token_count + 1
                end
                if token_count ~= self_.accounts then
                    table.insert(self_.reader_errors,
                        string.format(
                            'SK scan: expected %d tokens, got %d',
                            self_.accounts, token_count))
                end
                self_.read_count = self_.read_count + 1
                box.commit()
            end)
            if not ok then
                pcall(box.rollback)
            end
            fiber.yield()
        end
    end))
end

-- Reader that mixes range scans (GE, LE) with point reads
-- on random subsets. Exercises partial scans and cache
-- chain building with gaps.
function BankTest:start_mixed_reader()
    local self_ = self
    local mode = os.getenv('VINYL_STRESS_MIXED_MODE') or 'all'
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            local ok = pcall(function()
                box.begin()
                -- Random partial PK scan (GE from a random key).
                local start = math.random(1, self_.accounts)
                local total_ge = 0
                local count_ge = 0
                if mode == 'all' or mode == 'ge' then
                    for _, tuple in
                            self_.s:pairs(start, {iterator = 'GE'}) do
                        total_ge = total_ge + tuple[2]
                        count_ge = count_ge + 1
                    end
                end
                -- Reverse scan (LE from the same key).
                local total_le = 0
                local count_le = 0
                if mode == 'all' or mode == 'lt' then
                    for _, tuple in
                            self_.s:pairs(start, {iterator = 'LT'}) do
                        total_le = total_le + tuple[2]
                        count_le = count_le + 1
                    end
                end
                -- Point reads for min/max.
                if mode == 'all' or mode == 'minmax' then
                    local mn = self_.s.index.pk:min()
                    local mx = self_.s.index.pk:max()
                    if mn == nil or mx == nil then
                        table.insert(self_.reader_errors,
                            'min/max returned nil')
                    end
                end
                -- GE includes start, LT excludes it.
                -- Together they cover all accounts.
                local combined = total_ge + total_le
                if mode == 'all' and
                   count_ge + count_le ~= self_.accounts then
                    table.insert(self_.reader_errors,
                        string.format(
                            'mixed: GE(%d)+LT(%d)=%d, expected %d',
                            count_ge, count_le,
                            count_ge + count_le,
                            self_.accounts))
                end
                if mode == 'all' and
                   combined ~= self_.expected_total then
                    table.insert(self_.reader_errors,
                        string.format(
                            'mixed: GE+LT total %d, expected %d',
                            combined, self_.expected_total))
                end
                self_.read_count = self_.read_count + 1
                box.commit()
            end)
            if not ok then
                pcall(box.rollback)
            end
            fiber.yield()
        end
    end))
end

function BankTest:start_snapshot()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            pcall(box.snapshot)
            fiber.sleep(0.5)
        end
    end))
end

function BankTest:start_ddl_nemesis()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            fiber.sleep(math.random() * 0.2)
            pcall(function()
                self_.s:create_index('nemesis',
                    {parts = {2, 'integer'}, unique = false,
                     if_not_exists = true})
            end)
            fiber.sleep(math.random() * 0.2)
            pcall(function()
                if self_.s.index.nemesis ~= nil then
                    self_.s.index.nemesis:drop()
                end
            end)
        end
    end))
end

function BankTest:start_wal_chaos()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            fiber.sleep(math.random() * 0.1)
            box.error.injection.set('ERRINJ_WAL_IO', true)
            fiber.yield()
            box.error.injection.set('ERRINJ_WAL_IO', false)
        end
    end))
end

function BankTest:start_ww_check_delay()
    local self_ = self
    table.insert(self.fibers, fiber.create(function()
        while not self_.stop do
            fiber.sleep(math.random() * 0.1)
            box.error.injection.set(
                'ERRINJ_VY_CHECK_CONCURRENT_WRITE_DELAY', true)
            fiber.yield()
            box.error.injection.set(
                'ERRINJ_VY_CHECK_CONCURRENT_WRITE_DELAY', false)
        end
    end))
end

function BankTest:run()
    for _ = 1, self.writers do self:start_writer() end
    -- Split readers across PK scan, SK scan, and mixed scan.
    local pk_readers = self.pk_readers or
        math.max(1, math.floor(self.readers / 3))
    local sk_readers = self.sk_readers or
        math.max(1, math.floor(self.readers / 3))
    local mixed_readers = self.mixed_readers or math.max(0,
        self.readers - pk_readers - sk_readers)
    for _ = 1, pk_readers do self:start_reader() end
    for _ = 1, sk_readers do self:start_sk_reader() end
    for _ = 1, mixed_readers do self:start_mixed_reader() end
    if self.snapshots then
        self:start_snapshot()
    end
    if self.ddl_nemesis then
        self:start_ddl_nemesis()
    end
    if self.wal_chaos and
       require('tarantool').build.target:find('Debug') then
        self:start_wal_chaos()
        self:start_ww_check_delay()
    end

    fiber.sleep(self.duration)
    self.stop = true
    for _, f in ipairs(self.fibers) do
        while f:status() ~= 'dead' do
            fiber.yield()
        end
    end
    if require('tarantool').build.target:find('Debug') then
        box.error.injection.set('ERRINJ_WAL_IO', false)
    end

    log.info('bank test: %d writes, %d reads, %d conflicts',
             self.write_count, self.read_count, self.conflict_count)
end

function BankTest:replay_xlog()
    local fio = require('fio')
    local xlog = require('xlog')
    local txns = {}
    local tsn_order = {}
    local xlogs = fio.glob(fio.pathjoin(box.cfg.wal_dir, '*.xlog'))
    table.sort(xlogs)
    for _, path in ipairs(xlogs) do
        for _, row in xlog.pairs(path) do
            if row.HEADER and row.BODY and
               row.BODY.space_id == self.s.id and
               row.BODY.tuple then
                local tsn = row.HEADER.tsn or row.HEADER.lsn
                if not txns[tsn] then
                    txns[tsn] = {}
                    table.insert(tsn_order, tsn)
                end
                table.insert(txns[tsn], {
                    key = row.BODY.tuple[1],
                    val = row.BODY.tuple[2],
                })
            end
        end
    end

    local accts = {}
    for i = 1, self.accounts do
        accts[i] = self.initial_balance
    end
    for _, tsn in ipairs(tsn_order) do
        local entries = txns[tsn]
        if #entries == 2 then
            local w1, w2 = entries[1], entries[2]
            local d1 = w1.val - (accts[w1.key] or 0)
            local d2 = w2.val - (accts[w2.key] or 0)
            if d1 + d2 ~= 0 then
                log.info('STALE tsn=%d: key %d: %d->%d '
                    .. 'key %d: %d->%d net=%d',
                    tsn, w1.key, accts[w1.key], w1.val,
                    w2.key, accts[w2.key], w2.val, d1 + d2)
            end
            accts[w1.key] = w1.val
            accts[w2.key] = w2.val
        elseif #entries == 1 then
            accts[entries[1].key] = entries[1].val
        end
    end
    local total = 0
    for _, v in pairs(accts) do total = total + v end
    return total
end

function BankTest:verify()
    local xlog_total = self:replay_xlog()
    log.info('bank test: xlog replay total = %d', xlog_total)

    -- Balance invariant.
    local total = 0
    local tokens = {}
    for _, tuple in self.s:pairs() do
        total = total + tuple[2]
        table.insert(tokens, tuple[3])
    end
    log.info('bank test: final total = %d (expected %d)',
             total, self.expected_total)
    t.assert_equals(total, self.expected_total,
        'final total must match initial total ' ..
        '(mismatch = lost update)')

    -- Token uniqueness.
    table.sort(tokens)
    for i = 2, #tokens do
        t.assert_not_equals(tokens[i], tokens[i - 1],
            'account tokens must be unique')
    end
    t.assert_equals(#tokens, self.accounts,
        'must have exactly ACCOUNTS tokens')

    -- Reader consistency.
    t.assert_equals(self.reader_errors, {},
        'all snapshot reads must see consistent total ' ..
        '(mismatch = torn scan)')

    -- Sanity.
    t.assert_gt(self.write_count, 0, 'writers must have committed')
    if self.readers > 0 then
        t.assert_gt(self.read_count, 0,
            'readers must have completed')
    end

    -- No read view leaks.
    local rv = box.stat.vinyl().tx.read_views
    t.assert_equals(rv, 0, 'no read view leaks')
end

return BankTest
