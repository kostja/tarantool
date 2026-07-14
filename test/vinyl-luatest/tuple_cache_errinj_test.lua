--
-- Error injection tests for the vinyl tuple cache.
-- Debug-only: uses box.error.injection.
-- The tests are grouped by topic:
--
-- 1. Unconfirmed data stays out of the cache
-- 2. Concurrent writes vs pending links
-- 3. Dead keys under concurrent change
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group('tuple_cache_errinj')

g.before_all(function(cg)
    t.tarantool.skip_if_not_debug()
    cg.server = server:new()
    cg.server:start()
end)

g.after_all(function(cg)
    if cg.server ~= nil then
        cg.server:drop()
    end
end)

g.after_each(function(cg)
    cg.server:exec(function()
        box.error.injection.set('ERRINJ_WAL_DELAY', false)
        box.error.injection.set('ERRINJ_WAL_WRITE_DISK', false)
        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        if box.space.test ~= nil then
            box.space.test:drop()
        end
    end)
end)

--
-- 1. Unconfirmed data stays out of the cache.
--
-- Prepared statements, and chains across them, never enter: a
-- retraction cannot reach into the cache, so nothing retractable
-- may rest in it.
--

-- A prepared statement never enters the tuple cache: a read-committed
-- reader sees it, but reading it must not populate the cache -- the
-- statement is retractable, and commit does not invalidate the
-- cache.
g.test_prepared_statement_not_cached = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}
        local write = helpers.suspended_write(function() s:replace{1, 'new'} end)

        local before = s.index.pk:stat().cache.put.rows
        box.begin({txn_isolation = 'read-committed'})
        t.assert_equals(s:get(1), {1, 'new'})
        t.assert_equals(s:select(), {{1, 'new'}})
        box.commit()
        local after = s.index.pk:stat().cache.put.rows
        t.assert_equals(after - before, 0,
            'a prepared statement must not enter the cache')

        -- After the write commits, reads cache it as usual.
        t.assert(write.commit(), 'writer must commit')
        before = s.index.pk:stat().cache.put.rows
        t.assert_equals(s:get(1), {1, 'new'})
        local put = s.index.pk:stat().cache.put.rows - before
        t.assert_gt(put, 0, 'a confirmed statement is cached')
    end)
end

-- Commit does not invalidate the cache; the value cached before the
-- write was prepared must not outlive the commit. The old value read
-- while the write sat prepared is not cached either: it is shadowed
-- by the prepared write above the reader's view and refused as
-- stale. The first read after the commit therefore sees the new
-- value and re-populates the cache.
g.test_no_stale_value_across_commit = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'old'}

        -- Cache the old value.
        t.assert_equals(s:get(1), {1, 'old'})

        local write = helpers.suspended_write(function() s:replace{1, 'new'} end)

        -- A reader at the confirmed view still sees the old value
        -- while the write is in flight.
        t.assert_equals(s:get(1), {1, 'old'})
        t.assert_equals(s:select(), {{1, 'old'}})

        t.assert(write.commit(), 'writer must commit')

        -- Every read after the commit sees the new value: no stale
        -- node survived the commit, no stale gap hides the key.
        t.assert_equals(s:get(1), {1, 'new'})
        t.assert_equals(s:select(), {{1, 'new'}})
    end)
end

-- A read-committed scan that consumes a prepared DELETE must not
-- record a chain across the deleted key: if the DELETE is rolled
-- back, the key is visible again, and a cached gap would hide it.
g.test_no_chain_across_prepared_delete_rollback = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{2, 'b'}
        s:replace{3, 'c'}

        local write = helpers.suspended_write(function() s:delete(2) end)

        -- The scan sees the prepared DELETE applied.
        box.begin({txn_isolation = 'read-committed'})
        t.assert_equals(s:select(), {{1, 'a'}, {3, 'c'}})
        box.commit()

        -- The DELETE fails its WAL write and is rolled back.
        t.assert_not(write.fail(), 'the delete must fail WAL')

        -- The key is alive; no cached gap may hide it.
        t.assert_equals(s:select(), {{1, 'a'}, {2, 'b'}, {3, 'c'}})
        t.assert_equals(s:get(2), {2, 'b'})
    end)
end

-- Same scan shape, but the DELETE commits: repeat scans return
-- correct results and the chain re-forms over the deleted key.
g.test_no_chain_across_prepared_delete_commit = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'a'}
        s:replace{2, 'b'}
        s:replace{3, 'c'}

        local write = helpers.suspended_write(function() s:delete(2) end)

        box.begin({txn_isolation = 'read-committed'})
        t.assert_equals(s:select(), {{1, 'a'}, {3, 'c'}})
        box.commit()

        t.assert(write.commit(), 'the delete must commit')

        t.assert_equals(s:select(), {{1, 'a'}, {3, 'c'}})
        t.assert_equals(s:get(2), nil)

        -- A repeat scan re-forms the chain: the second scan is
        -- served from the cache, touching neither the memory
        -- level nor the disk.
        t.assert_equals(s:select(), {{1, 'a'}, {3, 'c'}})
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(), {{1, 'a'}, {3, 'c'}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the repeat scan is served from the cache')
    end)
end

-- A rolled-back secondary-key move leaves no trace in either
-- cache: the prepared move invalidated the old key's entry, the
-- rollback invalidates the new key's, and the old row is read
-- from the deeper sources once. The repeat scan is then served
-- from the secondary cache, with no primary lookup.
g.test_secondary_move_rollback_restores_row = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 30}
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {2, 30}})

        local write = helpers.suspended_write(function() s:replace{1, 20} end)
        t.assert_not(write.fail(), 'the move must fail WAL')

        t.assert_equals(s.index.sk:select{}, {{1, 10}, {2, 30}})
        t.assert_equals(s.index.sk:select{20}, {})
        local sk = s.index.sk:stat()
        local pk = s.index.pk:stat()
        local flat = {sk.memory.iterator.lookup,
                      sk.disk.iterator.lookup,
                      pk.memory.iterator.lookup,
                      pk.disk.iterator.lookup}
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {2, 30}})
        sk = s.index.sk:stat()
        pk = s.index.pk:stat()
        t.assert_equals({sk.memory.iterator.lookup,
                         sk.disk.iterator.lookup,
                         pk.memory.iterator.lookup,
                         pk.disk.iterator.lookup}, flat,
            'the repeat scan is served from both caches')
    end)
end

--
-- 2. Concurrent writes vs pending links.
--
-- A write aborts every pending link extended over it, reaching
-- across chain metadata exactly as far as completion can hop.
--

-- A write aborts every pending link extended over it, and the
-- abortion must reach across chain metadata, because completion
-- does: here two consecutive bound keys stand between the suspended
-- reader's pending link and the write, and the completion walk
-- would hop them both.
g.test_write_aborts_pending_link_across_consecutive_bounds =
        function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned'}},
                              unique = false, bloom_fpr = 1})
        -- {2, 30}: the primary row lives on disk, so its
        -- resolution yields on the point lookup delay. {4, 15}
        -- and {5, 17} become secondary orphans: the moves below
        -- leave the old keys in the secondary run, where the
        -- empty scans find them and yield on the page read delay.
        s:replace{2, 30}
        s:replace{4, 15}
        s:replace{5, 17}
        box.snapshot()
        -- The moves and the first row stay in memory: their
        -- resolutions terminate before the disk scan and do not
        -- yield.
        s:replace{4, 150}
        s:replace{5, 170}
        s:replace{1, 10}

        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', true)

        -- The scan caches {1, 10}, opens its pending link,
        -- proves the orphans at 15 and 17 dead from memory, and
        -- yields resolving secondary key 30: its walk is past the
        -- position the write below lands on.
        local lookups = s.index.pk:stat().lookup
        local scan_done = fiber.channel(1)
        fiber.create(function()
            box.begin({txn_isolation = 'read-committed'})
            local r = s.index.sk:select({}, {fullscan = true})
            box.commit()
            scan_done:put(r)
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.pk:stat().lookup, lookups + 4)
        end)

        -- Two empty partial-EQ scans insert their start bounds
        -- at 15 and 17 between the suspended scan's last result and
        -- the write below, and yield on the secondary run read:
        -- two consecutive metadata entries for the abortion to
        -- cross.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        local disk = s.index.sk:stat().disk.iterator.lookup
        local empty1 = fiber.channel(1)
        local empty2 = fiber.channel(1)
        fiber.create(function()
            empty1:put(s.index.sk:select({15}, {iterator = 'EQ'}))
        end)
        fiber.create(function()
            empty2:put(s.index.sk:select({17}, {iterator = 'EQ'}))
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.sk:stat().disk.iterator.lookup,
                        disk + 2)
        end)

        -- The write lands behind both bounds. Its abortion must
        -- walk across them to the suspended scan's last result, or
        -- the completion below hops them and claims the write's
        -- key absent.
        s:replace{3, 20}

        -- The empty scans close: their candidates resolve dead
        -- from memory. Both bounds stay: an aborted start bound
        -- is not taken back.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        t.assert_equals(empty1:get(), {})
        t.assert_equals(empty2:get(), {})

        -- The suspended scan resumes; its pending link died with
        -- the write, so its chain must not complete.
        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        t.assert_equals(scan_done:get(),
                        {{1, 10}, {2, 30}, {4, 150}, {5, 170}})

        -- No chain claim may span the write: a latest scan must
        -- see it.
        t.assert_equals(s.index.sk:select({}, {fullscan = true}),
                        {{1, 10}, {3, 20}, {2, 30}, {4, 150},
                         {5, 170}})
        box.cfg{vinyl_defer_deletes = defer}
    end)
end

-- The minimal shield: a single bound key between the suspended
-- reader's pending link and the write. Nothing is ever taken
-- back -- the write aborts the bound's own pending link, so the
-- bound stays -- and the completion walk would simply hop it.
g.test_write_aborts_shielded_pending_link = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned'}},
                              unique = false, bloom_fpr = 1})
        s:replace{2, 30}
        s:replace{5, 17}
        box.snapshot()
        -- The move and the first row stay in memory: their
        -- resolutions terminate before the disk scan and do not
        -- yield.
        s:replace{5, 170}
        s:replace{1, 10}

        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', true)

        -- The scan caches {1, 10}, opens its pending link,
        -- proves the orphan at 17 dead from memory, and yields
        -- resolving secondary key 30: its walk is past the
        -- position the write below lands on.
        local lookups = s.index.pk:stat().lookup
        local scan_done = fiber.channel(1)
        fiber.create(function()
            box.begin({txn_isolation = 'read-committed'})
            local r = s.index.sk:select({}, {fullscan = true})
            box.commit()
            scan_done:put(r)
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.pk:stat().lookup, lookups + 3)
        end)

        -- One empty partial-EQ scan inserts its start bound at
        -- 17 and yields on the secondary run read.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        local disk = s.index.sk:stat().disk.iterator.lookup
        local empty = fiber.channel(1)
        fiber.create(function()
            empty:put(s.index.sk:select({17}, {iterator = 'EQ'}))
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.sk:stat().disk.iterator.lookup,
                        disk + 1)
        end)

        -- The write lands behind the bound: the abortion must
        -- cross it to reach the suspended scan's pending link.
        s:replace{3, 20}

        -- The empty scan closes; its aborted bound stays.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        t.assert_equals(empty:get(), {})

        -- The suspended scan resumes; its pending link died with
        -- the write, so its chain must not complete.
        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        t.assert_equals(scan_done:get(),
                        {{1, 10}, {2, 30}, {5, 170}})

        -- No chain claim may span the write: a latest scan must
        -- see it.
        t.assert_equals(s.index.sk:select({}, {fullscan = true}),
                        {{1, 10}, {3, 20}, {2, 30}, {5, 170}})
        box.cfg{vinyl_defer_deletes = defer}
    end)
end

-- Two scans share one start bound: the second re-stamps it at
-- its creation. When the first scan is abandoned, its close must
-- leave the bound alone -- the slot carries the other scan's id
-- now -- or the survivor's chain would lose its start and serve
-- nothing. The abandonment lands while the survivor yields in
-- its page read, so the bound is not yet linked and only the
-- ownership check protects it: the timed page delay leaves a
-- wide window between the victim's wake-up and the survivor's.
g.test_abandoned_scan_keeps_shared_bound = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}},
                              bloom_fpr = 1})
        s:replace{1, 1}
        s:replace{3, 1}
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_READ_PAGE_TIMEOUT', 1.5)
        local disk0 = s.index.pk:stat().disk.iterator.lookup
        -- The victim: an empty partial EQ, suspended on the run
        -- read past its start bound, cancelled while it sleeps.
        -- The cancel is delivered when its page read returns.
        local victim = fiber.new(function()
            s:select({2})
        end)
        victim:set_joinable(true)
        fiber.yield()
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.pk:stat().disk.iterator.lookup,
                        disk0 + 1)
        end)
        -- The survivor: the same start key, re-stamping the
        -- shared bound, suspended in its own page read, which
        -- starts -- and so ends -- after the victim's.
        local survived = fiber.channel(1)
        fiber.create(function()
            survived:put(s:select({2}, {iterator = 'GE'}))
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.pk:stat().disk.iterator.lookup,
                        disk0 + 2)
        end)
        victim:cancel()
        victim:join()
        box.error.injection.set('ERRINJ_VY_READ_PAGE_TIMEOUT', 0)
        t.assert_equals(survived:get(), {{3, 1}})
        -- The survivor's chain kept its start: the very first
        -- repeat of the range, and the empty EQ, are served from
        -- the cache -- had the abandonment retracted the shared
        -- bound, the chain would have no start to serve from.
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select({2}, {iterator = 'GE'}), {{3, 1}})
        t.assert_equals(s:select({2}), {})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the shared bound survived the abandonment')
    end)
end

-- A scan at the latest view proves a deferred-delete orphan
-- dead and yields on a disk read before its end of matches. A
-- writer commits a fresh row between the scan's position and
-- the end of matches: the row's primary key is one the scan
-- never read, so the write conflicts with nothing and the
-- scan keeps its read view. The resumed scan must not claim
-- the range it did not observe: the new row lives in it.
g.test_scan_does_not_claim_row_inserted_while_suspended = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local pad = string.rep('x', 100)
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        -- One secondary key per run page: the suspending read.
        s:create_index('sk', {unique = false, page_size = 32,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        -- Two orphans-to-be on separate run pages: the second
        -- gives the scan a disk read to yield on after it has
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
        -- yields on the page holding the second.
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', true)
        go:put(true)
        -- One yield runs the reader until it blocks on the
        -- delayed disk read.
        fiber.yield()
        -- The fresh row's key precedes the suspended scan's
        -- position: the resumed walk may skip the row, but must
        -- not claim its range. The row's primary key is new to
        -- the scan, so the write conflicts with none of the
        -- scan's reads.
        s:replace{2, 4, 10, pad}
        box.error.injection.set('ERRINJ_VY_READ_PAGE_DELAY', false)
        t.assert_equals(paused:get(), {5})
        reader:join()
        -- The new row is alive and must be served.
        t.assert_equals(s.index.sk:select{4},
                        {{5, 4, 4, pad}, {2, 4, 10, pad}})
        box.cfg{vinyl_defer_deletes = defer}
    end)
end

--
-- 3. Dead keys under concurrent change.
--
-- A proven-dead verdict applies to the row version it was made
-- about: a key resurrected during the resolution's yield is
-- spared.
--

-- A proven-dead removal proves death of one version. The
-- resolution yields, and the key may be resurrected and re-cached
-- as a newer live row meanwhile: the removal must leave the live
-- entry alone.
g.test_dead_key_removal_spares_resurrected_entry = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {{2, 'unsigned'}},
                              unique = false, bloom_fpr = 1})
        s:replace{1, 77}
        box.snapshot()
        -- Orphan the secondary key: the row moves to another key
        -- and, with deferred DELETEs, the secondary keeps {77}
        -- until compaction. Dump again so the reader below yields
        -- on the disk lookup.
        s:replace{1, 88}
        box.snapshot()

        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', true)

        -- The reader walks over the orphan before the
        -- resurrection below -- so its walk saw nothing newer --
        -- and yields proving the orphan dead against the on-disk
        -- {1, 88}. The explicit transaction pins the view, so
        -- the verdict stands after the resurrection.
        local lookups = s.index.pk:stat().lookup
        local dead_done = fiber.channel(1)
        fiber.create(function()
            box.begin()
            local r = s.index.sk:select({77})
            box.commit()
            dead_done:put(r)
        end)
        t.helpers.retrying({}, function()
            t.assert_ge(s.index.pk:stat().lookup, lookups + 1)
        end)

        -- The key is resurrected and the live row re-cached: the
        -- range read and its resolution terminate in memory and
        -- do not yield.
        s:replace{1, 77}
        t.assert_equals(s.index.sk:select({77}), {{1, 77}})
        local gets = s.index.sk:stat().cache.get.rows

        -- The suspended reader resumes: at its view the key is dead
        -- and the result is empty. The removal must spare the
        -- newer live entry.
        box.error.injection.set('ERRINJ_VY_POINT_LOOKUP_DELAY', false)
        t.assert_equals(dead_done:get(), {})

        -- The live entry still serves.
        t.assert_equals(s.index.sk:select({77}), {{1, 77}})
        t.assert_gt(s.index.sk:stat().cache.get.rows, gets,
                    'the resurrected entry survived the dead-key '
                    .. 'removal')
        box.cfg{vinyl_defer_deletes = defer}
    end)
end
