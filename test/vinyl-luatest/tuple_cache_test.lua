--
-- Vinyl tuple cache tests.
-- The tests are grouped by topic:
--
-- 1. Admission and repeat service
-- 2. Chain building and service
-- 3. DELETE recording and fusion
-- 4. Invalidation
-- 5. Eviction, heat and quota
-- 6. Secondary index coherency
-- 7. Restore and cache version
-- 8. Slice bounds
--
local server = require('luatest.server')
local t = require('luatest')

local g = t.group('tuple_cache')

g.before_all(function(cg)
    cg.server = server:new()
    cg.server:start()
    cg.default_cache = cg.server:exec(function()
        return box.cfg.vinyl_cache
    end)
end)

g.after_all(function(cg)
    if cg.server ~= nil then
        cg.server:drop()
    end
end)

-- Record the fibers alive before the test, so after_each can tell
-- the test's own fibers from the server's service fibers.
g.before_each(function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        helpers.snap_base_fibers()
    end)
end)

-- A test that fails mid-body leaves its holder fibers suspended in
-- open transactions, whose read views pin the tx manager horizon
-- and cascade confusing failures into later tests. Cancel any
-- fiber the test spawned -- a suspended fiber unwinds and rolls its
-- transaction back -- then assert no read view survived, and
-- reset the cache size and drop the user spaces the test left.
g.after_each(function(cg)
    cg.server:exec(function(default_cache)
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        helpers.kill_test_fibers()
        t.helpers.retrying({timeout = 5}, function()
            t.assert_equals(box.stat.vinyl().tx.read_views, 0,
                'a test leaked an open transaction')
        end)
        box.cfg{vinyl_cache = default_cache}
        local names = {}
        for name, space in pairs(box.space) do
            if type(name) == 'string' and type(space) == 'table' and
               space.id ~= nil and space.id >= 512 then
                table.insert(names, name)
            end
        end
        for _, name in ipairs(names) do
            if box.space[name] ~= nil then
                box.space[name]:drop()
            end
        end
    end, {cg.default_cache})
end)

--
-- 1. Admission and repeat service.
--
-- Committed data enters the cache once: a repeat read is a pure
-- hit, the same tuple object gets an entry per index and per
-- multikey entry, and a miss leaves no trace.
--

-- A repeat read of cached data must not re-insert it: the second
-- pass over the same keys leaves the cache put counters flat, both
-- for scans and for point reads.
g.test_repeat_reads_do_not_reput = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i, 'v'}
        end

        t.assert_equals(#s:select(), 10)
        t.assert_equals(s:get(5), {5, 'v'})
        local puts = s.index.pk:stat().cache.put.rows
        t.assert_equals(#s:select(), 10)
        t.assert_equals(s:get(5), {5, 'v'})
        t.assert_equals(s.index.pk:stat().cache.put.rows, puts,
            'repeat reads must not re-insert cached statements')
    end)
end

-- A scan that finds nothing leaves nothing: an empty range is
-- not worth remembering, so a flood of misses on distinct keys
-- neither grows the cache nor displaces its content.
g.test_misses_leave_nothing = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        s:replace{0, 0}
        t.assert_equals(s:select{0}, {{0, 0}})
        local rows = s.index.pk:stat().cache.rows
        local mem = box.stat.vinyl().memory.tuple_cache
        -- A partial-key select missing on a fresh key each time.
        for i = 1, 1000 do
            t.assert_equals(s:select{i}, {})
        end
        t.assert_equals(s.index.pk:stat().cache.rows, rows)
        t.assert_equals(box.stat.vinyl().memory.tuple_cache, mem)
    end)
end

-- The same tuple object backs an entry in the primary cache and
-- in a secondary cache. Membership in one cache must not suppress
-- the insert into the other: the first secondary read populates
-- the secondary cache even though the primary cache already holds
-- the same object.
g.test_secondary_cache_not_starved = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {parts = {2, 'unsigned'}, unique = true})
        s:replace{1, 10, 'v'}

        local puts = s.index.sk:stat().cache.put.rows
        t.assert_equals(s.index.sk:get(10), {1, 10, 'v'})
        t.assert_gt(s.index.sk:stat().cache.put.rows, puts,
            'the first secondary read must populate the secondary '
            .. 'cache')
        puts = s.index.sk:stat().cache.put.rows
        t.assert_equals(s.index.sk:get(10), {1, 10, 'v'})
        t.assert_equals(s.index.sk:stat().cache.put.rows, puts,
            'the second secondary read is a pure cache hit')
    end)
end

-- Several multikey index entries share one tuple object; each entry
-- gets its own cache entry, and repeat reads of any of them are
-- pure hits.
g.test_multikey_entries_cached_and_not_reput = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{'[3][*]', 'unsigned'}}})
        s:replace{1, 'v', {10, 20}}

        t.assert_equals(s.index.sk:select(10), {{1, 'v', {10, 20}}})
        t.assert_equals(s.index.sk:select(20), {{1, 'v', {10, 20}}})
        local puts = s.index.sk:stat().cache.put.rows
        t.assert_equals(s.index.sk:select(10), {{1, 'v', {10, 20}}})
        t.assert_equals(s.index.sk:select(20), {{1, 'v', {10, 20}}})
        t.assert_equals(s.index.sk:stat().cache.put.rows, puts,
            'repeat multikey reads are pure cache hits')
    end)
end

--
-- 2. Chain building and service.
--
-- A scan links its results as it produces them: bound keys mark
-- the searched range's ends, resumed pages link back across
-- their resume position, a covered repeat scan is served without
-- touching the deeper sources, and redundant bounds fuse away.
--

-- An exhausted scan links its last result to the far end of the
-- key space regardless of the iterator type: a repeat of an
-- exclusive-key scan is served from the cache, tail included.
g.test_exclusive_scan_end_of_matches_cached = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i, 'v'}
        end
        local gt = s:select(3, {iterator = 'GT'})
        t.assert_equals(#gt, 7)
        local lt = s:select(8, {iterator = 'LT'})
        t.assert_equals(#lt, 7)
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(3, {iterator = 'GT'}), gt)
        t.assert_equals(s:select(8, {iterator = 'LT'}), lt)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a repeat exclusive-key scan must be served from the cache')
    end)
end

-- A read resumed after the previous page's last tuple opens its
-- leading pending link on that tuple's cache entry, so the pages
-- link up and a repeat of the resumed page is served from the
-- cache without touching the deeper sources.
g.test_resumed_read_links_across_pages = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i, 'v'}
        end
        local page1 = s:select(nil, {limit = 5})
        t.assert_equals(#page1, 5)
        local page2 = s:select(nil, {after = page1[5], limit = 5})
        t.assert_equals(#page2, 5)
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(nil, {after = page1[5], limit = 5}), page2)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a repeat resumed page must be served from the cache')

        -- An uncached resume position starts the chain at its own
        -- bound key entry: invalidate the first page's last row,
        -- read the second page twice -- the repeat must be served
        -- from the cache again.
        s:replace{5, 'w'}
        local page2b = s:select(nil, {after = page1[5], limit = 5})
        t.assert_equals(page2b, page2)
        stat = s.index.pk:stat()
        mem = stat.memory.iterator.lookup
        disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(nil, {after = page1[5], limit = 5}),
                        page2)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a page resumed after an uncached position must be '..
            'served from the cache')
    end)
end

-- The reverse mirror of the resumed-read case: a reverse page
-- resumed after the previous page's last tuple lands, on repeat,
-- exactly on that tuple -- one step above its own resume
-- position, across a provably empty range. That crossing must
-- not clear the stop, so the repeat of the resumed reverse page is
-- served from the cache without touching the deeper sources.
g.test_reverse_resumed_read_links_across_pages = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i, 'v'}
        end
        -- LE and LT resume from the top of the whole key space.
        for _, it in ipairs({'LE', 'LT'}) do
            local page1 = s:select(nil, {iterator = it, limit = 5})
            t.assert_equals(#page1, 5)
            local page2 = s:select(nil, {iterator = it,
                                         after = page1[5], limit = 5})
            t.assert_equals(#page2, 5)
            local stat = s.index.pk:stat()
            local mem = stat.memory.iterator.lookup
            local disk = stat.disk.iterator.lookup
            t.assert_equals(s:select(nil, {iterator = it,
                                           after = page1[5],
                                           limit = 5}), page2)
            stat = s.index.pk:stat()
            t.assert_equals({stat.memory.iterator.lookup,
                             stat.disk.iterator.lookup}, {mem, disk},
                ('a repeat resumed %s page must be served from the '..
                 'cache'):format(it))
        end
    end)
end

-- The REQ mirror: a reverse EQ over a multi-row key prefix,
-- resumed after the previous page's last tuple, is served from
-- the cache on repeat -- the empty way in from that tuple to
-- its resume position must not clear the stop.
g.test_reverse_eq_resumed_read_served = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        for i = 1, 10 do
            s:replace{1, i}
        end
        local page1 = s:select({1}, {iterator = 'REQ', limit = 5})
        t.assert_equals(#page1, 5)
        local page2 = s:select({1}, {iterator = 'REQ',
                                     after = page1[5], limit = 5})
        t.assert_equals(#page2, 5)
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select({1}, {iterator = 'REQ',
                                       after = page1[5], limit = 5}),
                        page2)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a repeat resumed REQ page must be served from the cache')
    end)
end

-- A scan served from the cache over existing links does not
-- link the served range again -- yet when the scan runs past
-- the cached range, the link at the boundary forms off the
-- served frontier and the chain extends: the repeat of the
-- longer scan is served entirely from the cache.
g.test_covered_scan_extends_chain = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 10 do
            s:replace{i}
        end
        -- Cache a prefix of the range.
        t.assert_equals(#s:select(nil, {limit = 5}), 5)
        -- Ride the cached prefix, then extend past it.
        t.assert_equals(#s:select{}, 10)
        -- The extension linked at the boundary: the repeat is
        -- served without touching the deeper sources.
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(#s:select{}, 10)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the boundary link forms off the served frontier')
    end)
end

-- Partial-key EQ end bounds do not accumulate: an EQ scan that
-- finds rows places its end bound right above its own last row,
-- so repeats deduplicate, and an EQ scan that finds nothing
-- leaves nothing at all.
g.test_eq_end_bounds_do_not_accumulate = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        s:replace{1, 1}
        s:replace{1000, 1}
        t.assert_equals(s:select{}, {{1, 1}, {1000, 1}})
        local overhead0 = s.index.pk:stat().cache.overhead.rows
        for i = 2, 201 do
            t.assert_equals(s:select{i}, {})
        end
        t.assert_equals(s.index.pk:stat().cache.overhead.rows,
                        overhead0,
            'empty partial-key EQ scans leave nothing behind')
        for _ = 1, 10 do
            t.assert_equals(s:select{1}, {{1, 1}})
        end
        t.assert_equals(s.index.pk:stat().cache.overhead.rows,
                        overhead0,
            'a matching EQ end bound fuses into the claimed '..
            'gap the moment it is placed')
    end)
end

-- A later full scan crosses the EQ end bound without fusing
-- it -- the bound lingers until eviction. The repeat
-- EQ is served regardless: the end of matches follows from the
-- links the crossing chain left behind.
g.test_repeat_eq_served_across_end_bound = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        s:replace{3, 1}
        s:replace{5, 1}
        s:replace{5, 2}
        s:replace{7, 1}
        box.snapshot()
        t.assert_equals(s:select{5}, {{5, 1}, {5, 2}})
        -- The full scan's chain links through the end bound.
        t.assert_equals(s:select{},
                        {{3, 1}, {5, 1}, {5, 2}, {7, 1}})
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{5}, {{5, 1}, {5, 2}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the repeat EQ scan is served from the cache without '..
            'its end bound')
    end)
end

-- A scan starting inside a claimed gap leaves no opening bound
-- behind: the bound fuses into the entry the claim starts from
-- -- here the tuple below the gap -- as it is inserted, and the
-- chain grows from that entry. Without the fuse, a gap between
-- two tuples would accumulate one key entry per distinct start
-- key, and every scan crossing the gap would pay a crossing per
-- entry.
g.test_gap_scan_bounds_fuse = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{1000}
        t.assert_equals(s:select{}, {{1}, {1000}})
        local overhead0 = s.index.pk:stat().cache.overhead.rows
        for i = 2, 201 do
            t.assert_equals(s:select({i}, {iterator = 'GT'}),
                            {{1000}})
        end
        local st = s.index.pk:stat().cache
        t.assert_equals(st.overhead.rows, overhead0,
            'scan starts inside a claimed gap leave no bound')
        t.assert_equals(s:select{}, {{1}, {1000}})
    end)
end

-- The reverse mirror: a reverse scan starting inside a claimed
-- gap leaves no opening bound behind either. The fuse is
-- direction-blind: the bound fuses into the entry the claim
-- starts from -- the tuple below the gap -- and the chain
-- grows from it.
g.test_gap_reverse_scan_bounds_fuse = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{1000}
        t.assert_equals(s:select({}, {iterator = 'LE'}),
                        {{1000}, {1}})
        local overhead0 = s.index.pk:stat().cache.overhead.rows
        for i = 999, 800, -1 do
            t.assert_equals(s:select({i}, {iterator = 'LT'}), {{1}})
        end
        local st = s.index.pk:stat().cache
        t.assert_equals(st.overhead.rows, overhead0,
            'scan starts inside a claimed gap leave no bound')
        t.assert_equals(s:select({}, {iterator = 'LE'}),
                        {{1000}, {1}})
    end)
end

-- A paginated scan resumes after a position, and the position
-- is only required to be a match of the searched key. A GT/LT
-- search with a partial key returns the keys past every match,
-- yet a position that is itself a match is admitted, and the
-- scan resumes among the matches -- keys a positionless scan of
-- the same type skips. Memtx is the reference for every case;
-- each query repeats, so the second pass is served over
-- whatever chain the first one recorded.
g.test_scan_resumes_after_position_inside_prefix = function(cg)
    cg.server:exec(function()
        local function fill(s)
            s:replace{2, 9}
            s:replace{3, 1}
            s:replace{3, 4}
            s:replace{3, 5}
        end
        local function check(s)
            for _ = 1, 2 do
                t.assert_equals(
                    s:select({3}, {iterator = 'GT', after = {3, 3},
                                   limit = 5}),
                    {{3, 4}, {3, 5}})
                t.assert_equals(
                    s:select({3}, {iterator = 'LT', after = {3, 4},
                                   limit = 5}),
                    {{3, 1}, {2, 9}})
                t.assert_equals(
                    s:select({3}, {iterator = 'EQ', after = {3, 4},
                                   limit = 5}),
                    {{3, 5}})
                local ok, err = pcall(s.select, s, {3},
                    {iterator = 'GE', after = {2, 5}, limit = 5})
                t.assert_not(ok, 'a position beyond the key '..
                                 'prefix is rejected')
                t.assert_str_contains(tostring(err),
                                      'position is invalid')
            end
        end
        local m = box.schema.space.create('reference')
        m:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        fill(m)
        check(m)
        m:drop()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        fill(s)
        check(s)
    end)
end

--
-- 3. DELETE recording and fusion.
--
-- A consumed DELETE above the horizon is recorded to serve
-- absence; at or below it, refused. Consecutive deletes fuse
-- into one entry holding the newest LSN, and a reader that
-- can still see under a fused key descends instead of
-- trusting the entry's links.
--

-- A scan whose trailing keys are all dead ends its chain on a
-- key entry: each consumed DELETE fuses into the adjacent
-- bound of an earlier scan as it is inserted. The bound
-- becomes the chain's frontier, and the close must handle a
-- frontier that is a key entry. Deferred deletes keep the
-- foreign bound linked across the dying keys: a primary-index
-- delete does not invalidate the secondary cache.
g.test_trailing_deletes_fuse_into_foreign_bound = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {
            engine = 'vinyl', defer_deletes = true})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 20}
        s:replace{3, 30}
        box.snapshot()
        -- Pin the horizon below the coming deletes.
        local pin = helpers.pin_read_view(function()
            s.index.sk:select{20}
            s.index.sk:select{30}
        end)
        -- Orphan secondary key 20 and let a narrow scan record
        -- its absence on its own start bound, linked to the
        -- still-live row of key 30.
        s:delete{2}
        t.assert_equals(s.index.sk:select({15}, {iterator = 'GE'}),
                        {{3, 30}})
        -- Orphan key 30 too: the bound stays linked, spanning a
        -- gap where both keys are now dead.
        s:delete{3}
        -- The full scan: after its last live row every key is
        -- dead, and their DELETEs fuse into the foreign bound.
        -- The fuse is witnessed by the entry counters: the dead
        -- keys' DELETEs leave no entries of their own, so the
        -- scan adds nothing beyond its own start bound.
        local overhead0 = s.index.sk:stat().cache.overhead.rows
        t.assert_equals(s.index.sk:select{}, {{1, 10}})
        t.assert_equals(
            s.index.sk:stat().cache.overhead.rows - overhead0, 1,
            'the trailing deletes fused instead of adding entries')
        -- The chain closed cleanly: repeat scans stay correct.
        t.assert_equals(s.index.sk:select{}, {{1, 10}})
        t.assert_equals(s.index.sk:select({15}, {iterator = 'GE'}),
                        {})
        pin.release()
    end)
end

-- Negative caching across a secondary index: a range scan
-- consumes the DELETE of a dead secondary key and records it
-- into its chain, linked to the live neighbors. Readers of the
-- dead key are then served its absence from the cache: the
-- linked DELETE ends their search with no descent and no
-- primary lookup. The index is non-unique -- a unique index
-- serves a full-key EQ through the point path, which never
-- consumes chains -- and the DELETE is dumped to disk before
-- the repeat reads, so the memory level cannot answer them
-- either: the absence can only come from the cache.
g.test_dead_secondary_key_skips_primary_lookup = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 20}
        s:replace{3, 30}
        box.snapshot()
        -- Pin the horizon below the coming delete: the holder
        -- reads the row the delete kills, so the delete
        -- displaces it into a read view, and the DELETE stays
        -- worth recording.
        local pin = helpers.pin_read_view(function() s:get{2} end)
        -- Kill secondary key 20.
        s:delete{2}
        -- The producer scan consumes the DELETE of key 20 and
        -- records it into the chain.
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {3, 30}})
        box.snapshot()
        -- Repeat readers of the dead key: absence served from
        -- the cache. An empty result yields no tuple, so the
        -- witness is the absent descent: the memory and disk
        -- levels are never consulted, nor is the primary index.
        local pk_lookup0 = s.index.pk:stat().lookup
        local sk_stat0 = s.index.sk:stat()
        local mem0 = sk_stat0.memory.iterator.lookup
        local disk0 = sk_stat0.disk.iterator.lookup
        for _ = 1, 5 do
            t.assert_equals(s.index.sk:select{20}, {})
        end
        local sk_stat = s.index.sk:stat()
        t.assert_equals({sk_stat.memory.iterator.lookup,
                         sk_stat.disk.iterator.lookup},
                        {mem0, disk0},
            'the cached DELETE spares the descent')
        t.assert_equals(s.index.pk:stat().lookup, pk_lookup0,
            'a proven-dead secondary key is served as absent '..
            'without a primary lookup')
        pin.release()
    end)
end

-- A dead secondary row nobody can see alive is dropped by the
-- scan that proves it dead. The state needs the primary and
-- secondary caches out of step: a delete defers its secondary
-- DELETE only when the old row is nowhere cheap to find, yet a
-- scan that caches the secondary row caches the primary row
-- too. An update of unindexed fields breaks the tie: it
-- invalidates the primary entry -- the primary is always
-- written -- and leaves the secondary entry untouched. The
-- move then defers, the stale secondary row lingers with no
-- write ever invalidating it, and the proving scan is the only
-- place that learns it is dead and drops it. The neighbors
-- link across the dead key and the repeat scan is served.
g.test_dead_row_dropped_at_horizon = function(cg)
    cg.server:exec(function()
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 20}
        s:replace{3, 30}
        box.snapshot()
        -- Cache the rows, the doomed one included -- and with
        -- them their primary rows.
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 20}, {3, 30}})
        -- Knock the primary entry out while the secondary entry
        -- stays: the unindexed update writes -- and invalidates
        -- -- the primary alone. The dump empties the memory
        -- level, so the move below finds the old row nowhere
        -- cheap and defers its secondary DELETE.
        s:replace{2, 20, 'x'}
        box.snapshot()
        s:replace{2, 25}
        -- The proving scan serves the stale cached row of key
        -- 20, learns from the primary that the row is dead for
        -- every reader, and drops it: no write will ever
        -- invalidate it.
        local rows0 = s.index.sk:stat().cache.rows
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 25}, {3, 30}})
        t.assert_le(s.index.sk:stat().cache.rows, rows0 + 1,
            'the stale row was dropped, not kept beside the '..
            'fresh one')
        -- The dropped row was the proving scan's own frontier,
        -- so the drop breaks that scan's chain: the next scan
        -- still meets the orphan in the run, proves it dead once
        -- more -- with nothing cached to drop -- and links the
        -- neighbors across it. The scan after is served.
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 25}, {3, 30}})
        local stat = s.index.sk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 25}, {3, 30}})
        stat = s.index.sk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the neighbors link across the dropped dead row')
        box.cfg{vinyl_defer_deletes = defer}
    end)
end

-- A DELETE whose LSN equals the oldest read view serves no
-- reader: the oldest reader sees the DELETE itself, so nobody
-- can see the row. It is not recorded into the chain, and the
-- chain is not broken either -- the neighbors link up across
-- the dead key, so the repeat scan is served from the cache.
g.test_delete_at_horizon_not_recorded = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {parts = {{2, 'unsigned'}}})
        local o = box.schema.space.create('other', {engine = 'vinyl'})
        o:create_index('pk')
        o:replace{1}
        s:replace{1, 10}
        s:replace{2, 20}
        s:replace{3, 30}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 20}, {3, 30}})
        -- The delete: key 20 dies at some LSN L.
        s:delete{2}
        -- Open a reader at exactly L: it reads the other space
        -- and is displaced by a write there, so its view is born
        -- with our delete as the last commit.
        local reader = helpers.suspended_reader(function() o:get{1} end)
        o:replace{1, 'displace'}
        -- The producer scan consumes the DELETE of key 20. The
        -- DELETE sits exactly at the horizon: even the oldest
        -- reader sees the key as absent, so it is not recorded,
        -- and the chain closes across the dead key.
        local overhead0 = s.index.sk:stat().cache.overhead.rows
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {3, 30}})
        t.assert_equals(s.index.sk:stat().cache.overhead.rows,
                        overhead0,
            'no DELETE entry is recorded at the horizon')
        -- The reader at the horizon agrees: the key is absent.
        t.assert_equals(reader.probe(function()
            return s.index.sk:select{20}
        end), {})
        reader.stop()
        -- The chain spans the dead key: the repeat is served.
        local stat = s.index.sk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {3, 30}})
        stat = s.index.sk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the neighbors link across the dead key')
        o:drop()
    end)
end

-- Consecutive deleted keys fuse into a single cache entry
-- carrying the newest of the deletes' LSNs: for any reader at or
-- above it the keys are simply absent, and a reader that could
-- still see one of them alive finds the entry invisible and
-- descends to the deeper sources.
g.test_deleted_range_fuses = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local N = 200
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, N + 1 do s:replace{i} end
        -- Pin the read-view horizon below the deletes: a reader
        -- displaced by the first delete stays open across all of
        -- them.
        local reader = helpers.suspended_reader(function() s:get{2} end)
        -- A forward scan caches the first delete it meets; the
        -- ascending order puts the newest LSN last, so every
        -- fused delete must raise the cached entry.
        for i = 2, N do s:delete{i} end
        t.assert_equals(s:select{}, {{1}, {N + 1}})
        -- The whole deleted run sits in one fused entry
        -- between the two survivors, plus the scan's own two
        -- bound keys.
        t.assert_le(s.index.pk:stat().cache.overhead.rows, 3)
        -- The old reader sees under the whole span: the query
        -- lands on the fused entry, invisible to it, so
        -- the deeper sources supply the still-visible row.
        t.assert_equals(reader.probe(function()
            return s:select({2}, {iterator = 'LE'})
        end), {{2}, {1}})
        reader.stop()
    end)
end

-- A scan's leading deletes fuse into its own start bound: the
-- bound's LSN rises and no DELETE entry enters the cache at
-- all. A reader that could still see a fused key alive does not
-- observe the bound's links -- whether it lands on the bound
-- or beyond it -- and descends to the deeper sources.
g.test_deletes_fuse_into_start_bound = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{6}
        s:replace{7}
        s:replace{10}
        local reader = helpers.suspended_reader(function() s:get{6} end)
        s:delete{6}
        s:delete{7}
        -- The scan consumes both deletes right after placing
        -- its start bound: both fuse into it.
        t.assert_equals(s:select({5}, {iterator = 'GE'}), {{10}})
        t.assert_equals(s.index.pk:stat().cache.overhead.rows, 2)
        -- The repeat scan is served from the cache.
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select({5}, {iterator = 'GE'}), {{10}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a repeat scan is served from the cache')
        -- The old reader: neither a landing beyond the raised
        -- bound nor a landing on it grants a stop.
        t.assert_equals(reader.probe(function()
            return s:select({7}, {iterator = 'EQ'})
        end), {{7}})
        t.assert_equals(reader.probe(function()
            return s:select({5}, {iterator = 'GE'})
        end), {{6}, {7}, {10}})
        reader.stop()
    end)
end

-- The reverse landing rule: a reverse seek steps away from the
-- entry it lands on without classifying it, so the entry's
-- visibility must witness the range it rules.
--
-- Keys 10, 18, 20, 30; a pinned reader, then 20 and 18 deleted,
-- both guarded. A reverse full scan builds the chain: it records
-- the DELETE of key 20 next to the tuple {30}, and the DELETE of
-- key 18 fuses into that entry, raising its LSN -- one entry,
-- del{20}, now guards key 18 while sitting above it:
--
--     []- -> {10} -> del{20} -> {30} -> []+
--
-- A later scan of the range below 19 places its start bound in
-- the ({10}, del{20}) gap, where the bound fuses into {10}, and
-- the seek lands on del{20} -- the first entry above the range.
-- The walk steps down from the landing without crossing it, so
-- only the landing's own visibility says whether the range below
-- is covered. A reader at or above the fused LSN takes the link
-- into {10} and is served; the pinned reader, below it, takes
-- nothing, descends, and finds key 18 alive.
g.test_reverse_landing_on_guarded_delete = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- A two-part key: a partial-key reverse scan takes the
        -- range path -- a full key on a unique index would be
        -- served through the exact-match point path, which never
        -- lands on chains.
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        s:replace{10, 1}
        s:replace{18, 1}
        s:replace{20, 1}
        s:replace{30, 1}
        -- The pin reads the first-deleted key: the displacement
        -- lands below both deletes, so both stay guarded and the
        -- second fuses into the first's entry.
        local reader = helpers.suspended_reader(function() s:get{20, 1} end)
        s:delete{20, 1}
        s:delete{18, 1}
        -- The reverse producer records the DELETE of key 20 and
        -- fuses the DELETE of key 18 into it: one entry above
        -- key 19 now guards key 18.
        t.assert_equals(s:select({}, {iterator = 'LE'}),
                        {{30, 1}, {10, 1}})
        -- A fresh scan of the range below 19 lands exactly on
        -- the guarding entry -- its own bound fuses away into
        -- the tuple below. The entry is visible, so the landing
        -- grants the links and the scan is served.
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select({19}, {iterator = 'LT'}),
                        {{10, 1}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the visible landing grants the links')
        -- The pinned reader lands on the same entry, invisible
        -- to it: no grant, and the descent finds the fused key
        -- alive.
        t.assert_equals(reader.probe(function()
            return s:select({19}, {iterator = 'LT'})
        end), {{18, 1}, {10, 1}},
            'the invisible landing withholds the links')
        reader.stop()
    end)
end

-- The reverse-scan form of the fuse: the surviving entry is the
-- previously cached delete, and the fused deletes leave no trace
-- at all.
g.test_deleted_range_fuses_reverse = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local N = 200
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, N + 1 do s:replace{i} end
        local pin = helpers.pin_read_view(function() s:get{N} end)
        -- Descending deletes put the newest LSN at the low end:
        -- the fuse must raise the surviving entry to it.
        for i = N, 2, -1 do s:delete{i} end
        t.assert_equals(s:select({}, {iterator = 'LE'}),
                        {{N + 1}, {1}})
        t.assert_le(s.index.pk:stat().cache.overhead.rows, 3)
        pin.release()
    end)
end

-- A DELETE cached by one scan does not break another scan's
-- chain: links form through it like through a key entry, so a
-- range spanning deletes cached at different times is still
-- served from the cache on a repeat scan. Fusion keeps working
-- across it too -- deletes fused past the resident entry raise
-- its LSN along with the entry they fused into, so a reader
-- that could still see under a fused delete does not observe
-- the links around the resident entry either.
g.test_chain_links_through_foreign_delete = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1}
        s:replace{10}
        s:replace{20}
        s:replace{25}
        s:replace{30}
        local pin = helpers.pin_read_view(function() s:get{10} end)
        s:delete{10}
        s:delete{20}
        -- The narrow scan caches only delete{20}.
        t.assert_equals(s:select({15}, {iterator = 'GE'}),
                        {{25}, {30}})
        -- A reader whose view sits between delete{20} and the
        -- coming delete{25}: it is displaced by the latter.
        local reader = helpers.suspended_reader(function() s:get{25} end)
        s:delete{25}
        -- The full scan: the deletes fuse into its own start
        -- bound, and it links through the resident entries of
        -- the other scans, raising their LSNs so they keep guarding
        -- the newer fused deletes -- crossing does not fuse
        -- them, so they linger until eviction.
        t.assert_equals(s:select{}, {{1}, {30}})
        t.assert_equals(s.index.pk:stat().cache.overhead.rows, 4)
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{}, {{1}, {30}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a repeat scan is served from the cache across a '..
            'delete cached by another scan')
        -- The old reader still sees key 25 alive: the fused
        -- delete{25} must not hide behind the resident entry's
        -- older LSN. A landing inside the resident entry's gap
        -- takes the initial stop from that entry alone:
        -- invisible to this reader, it grants none, and the
        -- probe must still surface the row.
        t.assert_equals(reader.probe(function()
            return s:select({22}, {iterator = 'GE'})
        end), {{25}, {30}})
        reader.stop()
        pin.release()
    end)
end

-- Raising a fused DELETE's LSN in place needs no cache version
-- bump: a reader paused across the fuse resumes at its old
-- position and still sees the fused key alive -- the raised LSN
-- makes the entry invisible to it, so it descends to the deeper
-- sources instead of trusting the entry's links.
g.test_fuse_does_not_disturb_paused_reader = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{5}
        s:replace{10}
        s:replace{20}
        -- An old reader keeps delete{10} above the horizon, so
        -- the delete is cached by the warm scan.
        local pin = helpers.pin_read_view(function() s:get{10} end)
        s:delete{10}
        t.assert_equals(s:select{}, {{5}, {20}})
        -- The paused reader: yields {5}, then waits while the
        -- fuse happens, then resumes. Reading {20} first pins
        -- its view below the coming delete{20}.
        local r_step = fiber.channel(1)
        local r_result = fiber.channel(1)
        local reader = fiber.create(function()
            box.begin()
            s:get{20}
            local rows = {}
            for _, tuple in s:pairs() do
                table.insert(rows, tuple:totable())
                if #rows == 1 then
                    r_result:put(rows[1])
                    r_step:get()
                end
            end
            box.commit()
            r_result:put(rows)
        end)
        reader:set_joinable(true)
        t.assert_equals(r_result:get(), {5})
        -- The fuse: delete{20} lands next to the cached
        -- delete{10} entry, whose LSN is raised in place; the
        -- reader's view sits between the two LSNs.
        s:delete{20}
        t.assert_equals(s:select{}, {{5}})
        t.assert_equals(s.index.pk:stat().cache.overhead.rows, 3)
        -- The reader resumes: key 10 is gone for it, key 20 is
        -- still alive for it.
        r_step:put(true)
        t.assert_equals(r_result:get(), {{5}, {20}})
        reader:join()
        pin.release()
    end)
end

-- The fuse matrix: one dataset, one space, a sequence of
-- scenarios covering both scan directions and the combinations
-- of the fused entry (plain key entry, key entry carrying a
-- DELETE's LSN, DELETE entry), the absorber (tuple, key entry,
-- DELETE entry) and the surrounding tuples, on a multi-part
-- index so partial-key scans are exercised. Every scenario that
-- moves a DELETE's LSN also proves the guard survived: a reader
-- pinned below the LSN still finds the deleted rows, a fresh
-- reader is served from the cache. To run a single scenario,
-- set `only` to its name below.
g.test_fuse_matrix = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'unsigned'}}})
        local CACHE = box.cfg.vinyl_cache
        local PREFIXES = {10, 30, 50, 70, 90}

        local function ensure_data()
            for _, p in ipairs(PREFIXES) do
                for q = 1, 2 do
                    if s:get{p, q} == nil then
                        s:replace{p, q}
                    end
                end
            end
        end

        -- All rows except the given prefixes, in key order.
        local function rows_except(...)
            local skip = {}
            for _, p in ipairs({...}) do
                skip[p] = true
            end
            local res = {}
            for _, p in ipairs(PREFIXES) do
                if not skip[p] then
                    table.insert(res, {p, 1})
                    table.insert(res, {p, 2})
                end
            end
            return res
        end

        local function flush_cache()
            box.cfg{vinyl_cache = 0}
            box.cfg{vinyl_cache = CACHE}
        end

        local function overhead()
            return s.index.pk:stat().cache.overhead.rows
        end

        local function lookups()
            local st = s.index.pk:stat()
            return st.memory.iterator.lookup +
                   st.disk.iterator.lookup
        end

        -- The read must not reach beyond the cache.
        local function served(fn)
            local l0 = lookups()
            fn()
            t.assert_equals(lookups(), l0,
                            'the repeat read is cache-served')
        end

        -- An open transaction pinned below coming deletes; its
        -- probes must keep seeing the rows those deletes hide.
        -- The reader reads the whole prefix up front: a vinyl
        -- read view freezes at the first write that conflicts
        -- with the reader's read set, so a key absent from that
        -- set is not held at the pre-delete state. The cache is
        -- flushed afterwards -- the read view is transaction
        -- state and outlives the flush -- so the scenario starts
        -- from a clean cache regardless of what the pin read.
        local function pin(prefix)
            local rd = helpers.suspended_reader(function() s:select(prefix) end)
            flush_cache()
            return {
                probe = function(k, opts)
                    return rd.probe(function()
                        return s:select(k, opts)
                    end)
                end,
                release = rd.stop,
            }
        end

        local scenarios = {}
        local function scenario(name, fn)
            table.insert(scenarios, {name = name, fn = fn})
        end

        -- A key entry carrying a live DELETE LSN sits above a
        -- tuple: the tuple cannot carry the guard, so a chain
        -- crossing the entry must leave it in place.
        scenario('guarded_survives_above_tuple', function()
            local h = pin({30})
            s:delete{30, 1}
            s:delete{30, 2}
            -- The start bound absorbs both deletes; the limit
            -- keeps the scan short of the end of the key space.
            t.assert_equals(
                s:select({20}, {iterator = 'GE', limit = 1}),
                {{50, 1}})
            t.assert_equals(overhead(), 1)
            t.assert_equals(s:select{}, rows_except(30))
            -- The full scan added -inf and +inf; the delete
            -- it recorded fused into the guarded bound at 20
            -- the moment it was placed above it, so no separate
            -- DELETE entry persists. The guarded bound survived
            -- above tuple {10, 2}.
            t.assert_equals(overhead(), 3)
            t.assert_equals(h.probe({30}), {{30, 1}, {30, 2}})
            served(function()
                t.assert_equals(s:select{}, rows_except(30))
            end)
            h.release()
        end)

        -- Two guarded key entries in one gap: the lower one
        -- survives above a tuple and becomes the absorber, the
        -- upper one fuses into it, handing over its LSN.
        scenario('absorber_advances_and_takes_lsn', function()
            local h = pin({30})
            s:delete{30, 1}
            t.assert_equals(
                s:select({15}, {iterator = 'GE', limit = 1}),
                {{30, 2}})
            s:delete{30, 2}
            t.assert_equals(
                s:select({25}, {iterator = 'GE', limit = 1}),
                {{50, 1}})
            t.assert_equals(overhead(), 2)
            t.assert_equals(s:select{}, rows_except(30))
            -- The bound at 25 fused into the bound at 15;
            -- -inf, +inf and the DELETE entry joined.
            t.assert_equals(overhead(), 4)
            -- The younger delete's LSN travelled to the
            -- surviving bound: the pinned reader still sees
            -- both rows.
            t.assert_equals(h.probe({30}), {{30, 1}, {30, 2}})
            served(function()
                t.assert_equals(s:select{}, rows_except(30))
            end)
            h.release()
        end)

        -- The DELETEs a reverse chain crosses are inside a
        -- range an earlier reverse scan claimed, its bound
        -- guarding the claim with the DELETEs' LSN. A recorded
        -- DELETE entry would split the claim and strip the
        -- guard from one half, so the DELETEs fuse into the
        -- guarded bound, their successor. The claim stays
        -- whole and the guard holds throughout: the pinned
        -- reader keeps seeing both rows.
        scenario('reverse_delete_fusing_guards', function()
            local h = pin({70})
            s:delete{70, 2}
            s:delete{70, 1}
            t.assert_equals(
                s:select({85}, {iterator = 'LT', limit = 1}),
                {{50, 2}})
            t.assert_equals(overhead(), 1)
            local rev = {}
            for i = #rows_except(70), 1, -1 do
                table.insert(rev, rows_except(70)[i])
            end
            t.assert_equals(s:select({}, {iterator = 'LE'}), rev)
            t.assert_equals(overhead(), 3)
            t.assert_equals(h.probe({70}), {{70, 1}, {70, 2}})
            served(function()
                t.assert_equals(s:select({}, {iterator = 'LE'}),
                                rev)
            end)
            h.release()
        end)

        -- The horizon gate: while a reader lives below a fused
        -- DELETE's LSN, the guard testifying that key's absence
        -- is retained -- the reader keeps seeing the deleted
        -- rows across re-scans that fuse away redundant plain
        -- bounds around it. Once the reader is gone, the guard
        -- is reclaimed and a fresh reader is served the emptiness
        -- from the cache.
        scenario('live_reader_retains_guard', function()
            local h = pin({30})
            s:delete{30, 1}
            s:delete{30, 2}
            t.assert_equals(s:select{}, rows_except(30))
            t.assert_equals(h.probe({30}), {{30, 1}, {30, 2}})
            local o = overhead()
            -- A re-scan whose plain start bound lands in the now
            -- claimed gap fuses it away, but the guard testifying
            -- {30}'s absence stays: the reader still sees both
            -- rows.
            t.assert_equals(
                s:select({15}, {iterator = 'GE', limit = 1}),
                {{50, 1}})
            t.assert_le(overhead(), o)
            t.assert_equals(h.probe({30}), {{30, 1}, {30, 2}})
            served(function()
                t.assert_equals(s:select{}, rows_except(30))
            end)
            -- The reader gone, the guard is no longer pinned: a
            -- fresh scan reclaims it, and a fresh reader is
            -- served {30}'s emptiness flat.
            h.release()
            t.assert_equals(s:select{}, rows_except(30))
            served(function()
                t.assert_equals(s:select{30}, {})
            end)
        end)

        -- No fusion without a claim: a bound placed next to
        -- a cached but unlinked tuple must stay.
        scenario('unclaimed_gap_keeps_bound', function()
            t.assert_equals(s:get{10, 1}, {10, 1})
            t.assert_equals(
                s:select({20}, {iterator = 'GE', limit = 1}),
                {{30, 1}})
            t.assert_equals(overhead(), 1,
                'no claim covers the gap below the bound')
            -- A wider scan crossing the lone bound does not
            -- fuse it; it lingers until eviction, harmless,
            -- and the scan is still served.
            t.assert_equals(s:select{}, rows_except())
            t.assert_equals(overhead(), 3)
            served(function()
                t.assert_equals(s:select{}, rows_except())
            end)
        end)

        -- Partial-key scan bounds fuse like full-key ones,
        -- including a bound landing right under a tuple sharing
        -- its prefix.
        scenario('partial_key_bounds_fuse', function()
            t.assert_equals(s:select{}, rows_except())
            t.assert_equals(overhead(), 2)
            t.assert_equals(#s:select({40}, {iterator = 'GE'}), 6)
            t.assert_equals(overhead(), 2)
            served(function()
                t.assert_equals(
                    #s:select({40}, {iterator = 'GE'}), 6)
            end)
            t.assert_equals(#s:select({50}, {iterator = 'GE'}), 6)
            t.assert_equals(overhead(), 2)
            served(function()
                t.assert_equals(
                    #s:select({50}, {iterator = 'GE'}), 6)
            end)
        end)

        -- Partial-key EQ end bounds do not materialize in a
        -- gap the full scan already claimed: the end bound
        -- fuses back into the last matching row the moment
        -- it is placed, so the count is unchanged and the end of
        -- matches is witnessed by the existing claim -- the
        -- repeat EQ is served. Both EQ directions.
        scenario('partial_eq_end_bounds_fused', function()
            t.assert_equals(s:select{}, rows_except())
            t.assert_equals(overhead(), 2)
            t.assert_equals(s:select{50}, {{50, 1}, {50, 2}})
            t.assert_equals(overhead(), 2,
                'the EQ end bound fused into the last row')
            served(function()
                t.assert_equals(s:select{50},
                                {{50, 1}, {50, 2}})
            end)
            t.assert_equals(
                s:select({50}, {iterator = 'REQ'}),
                {{50, 2}, {50, 1}})
            t.assert_equals(overhead(), 2)
            served(function()
                t.assert_equals(
                    s:select({50}, {iterator = 'REQ'}),
                    {{50, 2}, {50, 1}})
            end)
        end)

        -- A whole prefix deleted under a pinned reader: the
        -- deletes fuse into a DELETE entry above a tuple,
        -- the old reader keeps seeing the rows, and a
        -- fresh partial-key EQ is served the emptiness from the
        -- cache.
        scenario('prefix_delete_guard_travels', function()
            t.assert_equals(s:select{}, rows_except())
            local h = pin({50})
            s:delete{50, 1}
            s:delete{50, 2}
            t.assert_equals(
                s:select({40}, {iterator = 'GE', limit = 1}),
                {{70, 1}})
            t.assert_equals(h.probe({50}), {{50, 1}, {50, 2}})
            t.assert_equals(s:select{50}, {})
            served(function()
                t.assert_equals(s:select{50}, {})
            end)
            t.assert_equals(h.probe({50}), {{50, 1}, {50, 2}})
            h.release()
        end)

        -- Set to a scenario name to run it alone.
        local only = nil
        for _, sc in ipairs(scenarios) do
            if only == nil or sc.name == only then
                flush_cache()
                ensure_data()
                local ok, err = pcall(sc.fn)
                if not ok then
                    local msg = type(err) == 'table' and
                                (err.message or
                                 require('json').encode(err)) or err
                    error(('scenario %s: %s'):format(sc.name, msg))
                end
            end
        end
    end)
end

-- A scan fuses the DELETEs it crosses into one chain entry
-- whose LSN becomes the newest of them: a reader above that
-- LSN sees the deleted keys as simply absent, a reader below
-- it must descend to the sources, where a row deleted above
-- its read view is still alive. Here rows {13, 5, 3} and
-- {12, 5, 2} are deleted in that order and a reader is pinned
-- between the deletes: it records the DELETE of {13, 5, 3} --
-- the one it sees -- into the fused range. The record must
-- not let the reader use the range's links: they are valid
-- only above the fused LSN, and {12, 5, 2} is still alive for
-- the reader.
g.test_read_below_fused_delete_sees_row = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{99, 9, 9}
        s:replace{11, 5, 1}
        s:replace{12, 5, 2}
        s:replace{13, 5, 3}
        s:replace{14, 5, 4}

        -- A snapshot reader holding a view: the first pins its
        -- view below both deletes, the second between them.
        local function pin_reader()
            local r = {go = fiber.channel(1), res = fiber.channel(1)}
            local pinned = fiber.channel(1)
            r.fiber = fiber.create(function()
                box.begin({txn_isolation = 'snapshot'})
                s:get{99}
                pinned:put(true)
                if r.go:get() then
                    r.res:put(s.index.sk:select({5, 4},
                                                {iterator = 'LE'}))
                end
                box.commit()
            end)
            r.fiber:set_joinable(true)
            t.assert(pinned:get())
            return r
        end

        local r0 = pin_reader()
        s:delete{13}
        local r2 = pin_reader()
        s:delete{12}

        -- A scan at the latest view fuses both DELETEs into
        -- its resume bound: the first page ends at {14, 5, 4},
        -- the second resumes after it, crosses the two deleted
        -- keys and links {11, 5, 1} to its start bound.
        t.assert_equals(s.index.sk:select({5, 4},
                        {iterator = 'LE', limit = 1}),
                        {{14, 5, 4}})
        t.assert_equals(s.index.sk:select({5, 4},
                        {iterator = 'LE', after = {14, 5, 4}}),
                        {{11, 5, 1}})

        -- The reader between the deletes sees the newer one as
        -- not yet happened: it must keep the row.
        r2.go:put(true)
        t.assert_equals(r2.res:get(),
                        {{14, 5, 4}, {12, 5, 2}, {11, 5, 1}})
        r2.fiber:join()
        r0.go:put(false)
        r0.fiber:join()
    end)
end

-- The forward variant of the same shape: the fusing scan, the
-- resume bound and the pinned reader all walk in the other
-- direction. The reader between the deletes must keep the row
-- deleted above its read view just the same.
g.test_forward_read_below_fused_delete_sees_row = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{99, 9, 9}
        s:replace{11, 5, 1}
        s:replace{12, 5, 2}
        s:replace{13, 5, 3}
        s:replace{14, 5, 4}

        local function pin_reader()
            local r = {go = fiber.channel(1), res = fiber.channel(1)}
            local pinned = fiber.channel(1)
            r.fiber = fiber.create(function()
                box.begin({txn_isolation = 'snapshot'})
                s:get{99}
                pinned:put(true)
                if r.go:get() then
                    r.res:put(s.index.sk:select({5, 1},
                                                {iterator = 'GE'}))
                end
                box.commit()
            end)
            r.fiber:set_joinable(true)
            t.assert(pinned:get())
            return r
        end

        local r0 = pin_reader()
        s:delete{13}
        local r2 = pin_reader()
        s:delete{12}

        -- The fusing scan walks forward: the first page ends
        -- at {11, 5, 1}, the second resumes after it and fuses
        -- the two DELETEs into its start bound.
        t.assert_equals(s.index.sk:select({5, 1},
                        {iterator = 'GE', limit = 1}),
                        {{11, 5, 1}})
        t.assert_equals(s.index.sk:select({5, 1},
                        {iterator = 'GE', after = {11, 5, 1}}),
                        {{14, 5, 4}, {99, 9, 9}})

        r2.go:put(true)
        t.assert_equals(r2.res:get(),
                        {{11, 5, 1}, {12, 5, 2}, {14, 5, 4},
                         {99, 9, 9}})
        r2.fiber:join()
        r0.go:put(false)
        r0.fiber:join()
    end)
end

-- A paginated scan resumes at its own start bound. The bound
-- key is shared with the bound an earlier scan left at the
-- same position, and a DELETE fused into that bound raised its
-- LSN. A resumed reader below the LSN must not take the
-- crossing into the bound as covered: the bound's link claims
-- a range holding a row the reader still sees alive.
g.test_resumed_read_below_fused_bound_sees_row = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'},
                                       {3, 'unsigned'}}})
        s:replace{99, 9, 9}
        s:replace{11, 5, 1}
        s:replace{12, 5, 2}
        s:replace{14, 5, 4}

        local go = fiber.channel(1)
        local res = fiber.channel(1)
        local pinned = fiber.channel(1)
        local reader = fiber.create(function()
            box.begin({txn_isolation = 'snapshot'})
            s:get{99}
            pinned:put(true)
            go:get()
            local rows = s.index.sk:select({5, 4},
                                           {iterator = 'LE',
                                            limit = 1})
            for _, tuple in ipairs(s.index.sk:select({5, 4},
                    {iterator = 'LE', after = rows[1]})) do
                table.insert(rows, tuple)
            end
            res:put(rows)
            box.commit()
        end)
        reader:set_joinable(true)
        t.assert(pinned:get())
        s:delete{12}

        -- A scan at the latest view fuses the DELETE into its
        -- resume bound and links {11, 5, 1} to the bound.
        t.assert_equals(s.index.sk:select({5, 4},
                        {iterator = 'LE', limit = 1}),
                        {{14, 5, 4}})
        t.assert_equals(s.index.sk:select({5, 4},
                        {iterator = 'LE', after = {14, 5, 4}}),
                        {{11, 5, 1}})

        -- The pinned reader resumes at the same position: the
        -- shared bound's LSN is above its read view, so it
        -- descends and keeps the row.
        go:put(true)
        t.assert_equals(res:get(),
                        {{14, 5, 4}, {12, 5, 2}, {11, 5, 1}})
        reader:join()
    end)
end

-- A snapshot reader proves a deferred-delete orphan dead at
-- its read view and records the DELETE into its chain. The row
-- was re-created under the same secondary key above the
-- reader's view, so the record holds at the reader's view
-- alone, and no later write will break the links around it. A
-- reader above the re-insert must not be served the record:
-- the key is alive again.
g.test_recorded_delete_does_not_hide_reinserted_key = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local defer = box.cfg.vinyl_defer_deletes
        box.cfg{vinyl_defer_deletes = true}
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
        box.cfg{vinyl_defer_deletes = defer}
    end)
end

--
-- 4. Invalidation.
--
-- A write removes its key's stale entry and refutes the links
-- and pending links its range covers; nothing a reader holds is
-- ever re-materialized.
--

-- A fresh key committed into a linked gap must split the one
-- link it lands in (the write-path invalidation's exact=false
-- case): the stop is a per-link claim. The repeat scan is
-- served over the intact links, descends exactly once for the
-- broken gap -- surfacing the new key -- and re-links it in
-- passing, so the third scan is served in full again.
g.test_fresh_key_in_linked_gap_invalidates = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        for i = 1, 5 do
            s:replace{i * 10}
        end
        -- One memory level and one run: the descent below costs
        -- exactly one seek into each.
        box.snapshot()
        -- Cache the chain with every gap claimed empty, and
        -- confirm the claims are served.
        t.assert_equals(s:select{},
                        {{10}, {20}, {30}, {40}, {50}})
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{},
                        {{10}, {20}, {30}, {40}, {50}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the empty gaps are served from the cache')
        -- Commit a key into one claimed gap: a key never cached,
        -- so the invalidation splits the one spanning link.
        s:replace{25}
        -- The repeat scan surfaces the fresh key and pays one
        -- descent, for the broken gap alone: the links around
        -- the other gaps keep their claims.
        stat = s.index.pk:stat()
        mem = stat.memory.iterator.lookup
        disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{},
                        {{10}, {20}, {25}, {30}, {40}, {50}},
            'the fresh key surfaces, the stale claim is gone')
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup - mem,
                         stat.disk.iterator.lookup - disk}, {1, 1},
            'only the broken gap descends')
        -- The descent re-linked the gap: the third scan is
        -- served in full.
        mem = stat.memory.iterator.lookup
        disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{},
                        {{10}, {20}, {25}, {30}, {40}, {50}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the repaired chain serves the scan')
    end)
end

-- A reader must not re-insert its last cached statement: the key
-- may be overwritten -- with the write fully committed -- between
-- two iterator steps, and its cache entry invalidated. Re-inserting
-- the stale statement when linking the next result would bring the
-- old value back as the latest, with nothing left to invalidate it.
g.test_reader_does_not_reinsert_overwritten_statement = function(cg)
    cg.server:exec(function()
        local fiber = require('fiber')
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        s:replace{1, 'v1'}
        s:replace{2, 'w'}

        -- A read-committed scan runs at the global read view and
        -- caches its results. Step it from Lua: the writes below land
        -- between its last cached statement and the next one.
        box.begin({txn_isolation = 'read-committed'})
        local gen, param, state = s:pairs()
        local r1, r2, r3
        state, r1 = gen(param, state)

        -- Overwrite the read key; the write commits and its
        -- invalidation removes the stale cache entry.
        local ch = fiber.channel(1)
        fiber.create(function()
            local ok = pcall(s.replace, s, {1, 'v2'})
            ch:put(ok)
        end)
        t.assert(ch:get(), 'overwriter must commit')

        state, r2 = gen(param, state)
        r3 = select(2, gen(param, state))
        box.commit()
        t.assert_equals(r1, {1, 'v1'})
        t.assert_equals(r2, {2, 'w'})
        t.assert_equals(r3, nil)

        -- No reader may see the stale {1,'v1'}.
        t.assert_equals(s:get(1), {1, 'v2'})
        t.assert_equals(s:select(), {{1, 'v2'}, {2, 'w'}})
    end)
end

-- A cached tombstone of a dead unique secondary key must not
-- hide the key's re-insertion under another primary key. A
-- full-key EQ on a unique index goes through the point path,
-- which never consumes chains, so the tombstone is recorded by
-- a range scan; the invalidation of the re-inserting write
-- cannot remove it -- the full keys differ -- and must break
-- the links around it instead. The lookup then finds the new
-- row, and the uniqueness check keeps rejecting a duplicate.
g.test_reinserted_key_found_after_cached_tombstone = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {1, 'unsigned'}})
        local i2 = s:create_index('i2', {parts = {2, 'unsigned'},
                                         unique = true})
        -- A suspended reader keeps the horizon below the DELETE,
        -- so the scan records the tombstone as a chain point
        -- instead of refusing it.
        local pin = helpers.pin_horizon()
        -- Neighbors, so the tombstone sits inside a chain.
        s:replace{100, 800}
        s:replace{101, 1000}
        s:replace{481, 900}
        s:delete{481}
        -- The range scan consumes the tombstone and records it,
        -- witnessed by the entry counter: the scan's two end
        -- bound keys plus the tombstone itself.
        local overhead0 = i2:stat().cache.overhead.rows
        t.assert_equals(i2:select({}, {iterator = 'GE'}),
                        {{100, 800}, {101, 1000}})
        t.assert_equals(i2:stat().cache.overhead.rows - overhead0,
                        3, 'the tombstone is recorded into the chain')
        -- A new row under another primary key takes over the
        -- unique key.
        s:replace{570, 900}
        -- The lookup must see it, and the uniqueness check must
        -- reject a third row with the same unique key.
        t.assert_equals(i2:select{900}, {{570, 900}})
        t.assert_equals(i2:get{900}, {570, 900})
        t.assert_error_msg_contains('Duplicate key exists',
                                    s.replace, s, {874, 900})
        pin.release()
    end)
end

--
-- 5. Eviction, heat and quota.
--
-- Use heats an entry; the sweep hand cools and evicts cold
-- ones. Evicting a guarded DELETE severs the chain. The
-- quota is a hard ceiling enforced by refusal, and dropped
-- caches are drained lazily.
--

-- Evicting a cold DELETE entry that still guards a reader must
-- break the chain at its place, not merge the neighbors' links:
-- a merged link would serve the key as absent to the reader
-- that still sees it alive. The reader pays a descent instead.
g.test_guarded_delete_eviction_severs_chain = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        box.cfg{vinyl_cache = 16 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        local churn = box.schema.space.create('churn',
                                              {engine = 'vinyl'})
        churn:create_index('pk')
        s:replace{1}
        s:replace{2}
        s:replace{3}
        local reader = helpers.suspended_reader(function() s:get{2} end)
        s:delete{2}
        -- The scan records the DELETE, linked to its neighbors.
        t.assert_equals(s:select{}, {{1}, {3}})
        -- Churn until the hand has swept the chain: it walks the
        -- tree in key order, cooling the once-used entries and
        -- evicting the cold ones -- the DELETE entry and the
        -- bound keys, at no heat, fall on the first pass. The
        -- point reads keep the tuples hot without crossing the
        -- chain.
        local pad = string.rep('x', 100)
        local i = 0
        t.helpers.retrying({timeout = 60}, function()
            for _ = 1, 50 do
                i = i + 1
                churn:replace{i, pad}
                churn:get(i)
            end
            s:get{1}
            s:get{3}
            t.assert_le(s.index.pk:stat().cache.rows, 3)
        end)
        -- The guard held: the eviction severed the chain at the
        -- removed entry's place instead of merging the neighbors'
        -- links, so the reader's range scan descends and still
        -- sees its row. A merged link would serve key 2 as
        -- absent.
        t.assert_equals(reader.probe(function()
            return s:select({1}, {iterator = 'GE'})
        end), {{1}, {2}, {3}})
        reader.stop()
        churn:drop()
    end)
end

-- Chain use keeps the chain hot: a scan heats every entry it
-- serves and the bounds it lands on and stops at, so a chain
-- scanned between the sweep hand's visits is never evicted and
-- its scans stay fully served from the cache, however long the
-- churn drives the sweep.
g.test_scanned_chain_survives_eviction = function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_cache = 16 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        local churn = box.schema.space.create('churn', {engine = 'vinyl'})
        churn:create_index('pk')
        local pad = string.rep('x', 100)
        for i = 1, 20 do
            s:replace{i, pad}
        end
        local expect = s:select()
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        for i = 1, 300 do
            churn:replace{i, pad}
            churn:get(i)
            t.assert_equals(s:select(), expect)
        end
        stat = s.index.pk:stat()
        t.assert_equals(stat.cache.evict.rows, 0,
            'nothing of the scanned space is evicted')
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'a scanned chain is fully served throughout the sweep')
        churn:drop()
    end)
end

-- Point lookups keep the tuples hot but never exercise the chain:
-- the sweep eventually reclaims the bounds, and the next range
-- scan must relink the range from the deeper sources -- once, and
-- correctly.
g.test_point_only_load_decays_chain = function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_cache = 32 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        local churn = box.schema.space.create('churn', {engine = 'vinyl'})
        churn:create_index('pk')
        local pad = string.rep('x', 100)
        for i = 1, 20 do
            s:replace{i, pad}
        end
        local expect = s:select()
        local next_churn = 1
        for _ = 1, 10 do
            for i = next_churn, next_churn + 49 do
                churn:replace{i, pad}
                churn:get(i)
            end
            next_churn = next_churn + 50
            for i = 1, 20 do
                t.assert_equals(s:get(i), {i, pad})
            end
        end
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(), expect)
        stat = s.index.pk:stat()
        t.assert(stat.memory.iterator.lookup > mem or
                 stat.disk.iterator.lookup > disk,
            'a chain never scanned must decay under the sweep')
        mem = stat.memory.iterator.lookup
        disk = stat.disk.iterator.lookup
        t.assert_equals(s:select(), expect)
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the relinked chain serves repeat scans')
        churn:drop()
    end)
end

-- GCLOCK eviction: a key that is read again and again stays hot
-- and survives the eviction hand, while cold keys churn through
-- the cache around it.
g.test_hot_key_survives_eviction = function(cg)
    cg.server:exec(function()
        box.cfg{vinyl_cache = 40 * 1024}
        -- The hot key lives in a space of its own: its cache
        -- stats are attributable, and the eviction hand's ring
        -- rotation visits its one-entry cache on every cycle, so
        -- nothing shields it but its heat.
        local hot = box.schema.space.create('hot', {engine = 'vinyl'})
        hot:create_index('pk')
        local cold = box.schema.space.create('cold',
                                             {engine = 'vinyl'})
        cold:create_index('pk')
        local pad = string.rep('x', 1000)
        hot:replace{1, pad}
        for i = 1, 100 do
            cold:replace{i, pad}
        end
        box.snapshot()

        -- Interleave hot-key reads with a cycling cold sweep:
        -- every round refreshes the hot key's heat, while the
        -- cold reads churn admissions that drive the hand.
        for i = 0, 299 do
            cold:get(i % 100 + 1)
            hot:get(1)
        end
        t.assert_gt(cold.index.pk:stat().cache.evict.rows, 0,
            'quota pressure must evict something')

        -- The hot key was read from disk exactly once and
        -- admitted exactly once: its repeat reads never missed,
        -- because its heat outlived every visit of the hand. A
        -- single eviction would cost a second lookup and a
        -- re-admission.
        local stat = hot.index.pk:stat()
        t.assert_equals({stat.disk.iterator.lookup,
                         stat.cache.put.rows}, {1, 1},
            'the hot key must survive eviction')
    end)
end

-- A row bigger than one walk's byte budget exhausts the walk
-- on the spot. The hand must still step past it and resume at
-- the next cache: were the next walk to restart at the same
-- entry, the row would cool once per admission instead of once
-- per cycle, and no heat survives that.
g.test_fat_entry_cooled_once_per_cycle = function(cg)
    cg.server:exec(function()
        -- A zero quota drains every leftover of the earlier
        -- tests: the sweep timing below assumes the ring holds
        -- nothing else.
        box.cfg{vinyl_cache = 0}
        box.cfg{vinyl_cache = 40 * 1024}
        local fat = box.schema.space.create('fat',
                                            {engine = 'vinyl'})
        fat:create_index('pk')
        local churn = box.schema.space.create('churn',
                                              {engine = 'vinyl'})
        churn:create_index('pk')
        fat:replace{1, string.rep('x', 600)}
        box.snapshot()
        -- Read the row into the cache and saturate its heat.
        for _ = 1, 16 do
            fat:get(1)
        end
        -- The churn admissions drive the hand for a fraction of
        -- the laps the saturated row is good for: it must still
        -- be resident.
        local pad = string.rep('x', 100)
        for i = 1, 270 do
            churn:replace{i, pad}
            churn:get(i)
        end
        t.assert_equals(fat.index.pk:stat().cache.evict.rows, 0,
            'a saturated fat row outlives a fraction of a cycle')
        churn:drop()
        fat:drop()
    end)
end

-- The eviction hand cools a row one lap per sweep, plus one
-- lap per doubling of its size above the resident mean: an
-- outlier sheds even saturated heat within a few visits and
-- cannot hog the cache, while an equally hot small row loses
-- one lap per visit and stays resident.
g.test_small_rows_outlive_large = function(cg)
    cg.server:exec(function()
        -- A zero quota drains every leftover of the earlier
        -- tests: the sweep timing below assumes the ring holds
        -- nothing else.
        box.cfg{vinyl_cache = 0}
        box.cfg{vinyl_cache = 40 * 1024}
        -- Each row lives in a space of its own: the cache stats
        -- are attributable, and the eviction hand's ring visits
        -- both one-entry caches on every cycle, so nothing
        -- shields a row but its heat.
        local small = box.schema.space.create('small',
                                              {engine = 'vinyl'})
        small:create_index('pk')
        local large = box.schema.space.create('large',
                                              {engine = 'vinyl'})
        large:create_index('pk')
        local churn = box.schema.space.create('churn',
                                              {engine = 'vinyl'})
        churn:create_index('pk')
        small:replace{1, 'x'}
        large:replace{1, string.rep('x', 16000)}
        box.snapshot()

        -- Read both rows into the cache and saturate their
        -- heat with an equal number of touches.
        for _ = 1, 16 do
            small:get(1)
            large:get(1)
        end

        -- Churn of small rows drives the hand and keeps the
        -- resident mean near the small row's size; neither row
        -- is touched again.
        local pad = string.rep('x', 100)
        local i = 0
        t.helpers.retrying({timeout = 60}, function()
            for _ = 1, 50 do
                i = i + 1
                churn:replace{i, pad}
                churn:get(i)
            end
            t.assert_gt(large.index.pk:stat().cache.evict.rows,
                        0,
                'the large row must be evicted despite its '..
                'heat')
        end)
        -- Churn another sweep's worth past the large row's
        -- eviction: the small row sheds one lap where the large
        -- row shed its whole heat, so nearly all of its heat
        -- remains. The read below is served from the cache --
        -- the row was read from disk exactly once and admitted
        -- exactly once.
        for _ = 1, 300 do
            i = i + 1
            churn:replace{i, pad}
            churn:get(i)
        end
        t.assert_equals(small:get(1), {1, 'x'})
        local stat = small.index.pk:stat()
        t.assert_equals({stat.disk.iterator.lookup,
                         stat.cache.put.rows}, {1, 1},
            'the equally hot small row must survive')
        churn:drop()
        large:drop()
        small:drop()
    end)
end

-- A cached DELETE outlives its purpose once every reader that could
-- see under it is gone: crossings stop heating it, and the
-- eviction hand takes it regardless of heat, fusing the chain --
-- while entries the scans keep positioning on stay hot and
-- resident.
g.test_expired_delete_retired = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        box.cfg{vinyl_cache = 16 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk')
        local churn = box.schema.space.create('churn', {engine = 'vinyl'})
        churn:create_index('pk')
        for i = 1, 10 do s:replace{i} end
        local pin = helpers.pin_read_view(function() s:get{2} end)
        for i = 2, 9 do s:delete{i} end
        t.assert_equals(s:select{}, {{1}, {10}})
        local overhead = s.index.pk:stat().cache.overhead.rows
        pin.release()
        -- The DELETE entry expired: scans no longer keep it
        -- warm, so admission churn sweeps it out while the
        -- scanned chain itself stays served.
        local pad = string.rep('x', 100)
        local next_churn = 1
        t.helpers.retrying({timeout = 60}, function()
            for i = next_churn, next_churn + 49 do
                churn:replace{i, pad}
                churn:get(i)
            end
            next_churn = next_churn + 50
            t.assert_equals(s:select{}, {{1}, {10}})
            t.assert_lt(s.index.pk:stat().cache.overhead.rows,
                        overhead)
        end)
        churn:drop()
    end)
end

-- The cache quota is hard: when the eviction walk cannot reclaim
-- room from caches full of hot entries, a fresh admission is
-- refused, so the cache never grows past the quota no matter how
-- hot the resident set is. Every known key is re-heated before
-- each admission, so the walk keeps meeting hot entries: the
-- refusals themselves are asserted, not only their effect.
g.test_cache_never_exceeds_quota = function(cg)
    cg.server:exec(function()
        local quota = 16 * 1024
        box.cfg{vinyl_cache = quota}
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk')
        local pad = string.rep('x', 200)
        -- Grow a fully hot resident set past the quota brim:
        -- every known key is re-heated before the next
        -- admission, so no cold block ever exists for the
        -- eviction walk to live in. Once the cache is at the
        -- brim, the walk meets only hot entries, reclaims
        -- nothing, and the admission is refused.
        for n = 1, 100 do
            s:replace{n, pad}
            s:get{n}
            for j = 1, n do s:get{j} end
            t.assert_le(box.stat.vinyl().memory.tuple_cache,
                        quota)
        end
        t.assert_gt(s.index.pk:stat().cache.reject, 0,
            'a full cache of hot entries refuses admissions')
    end)
end

-- Dropping a space detaches its cache instead of walking it on
-- the TX thread: a bounded batch is freed inline, the remainder
-- is stowed and consumed by the eviction walk -- a quota change
-- walks it away completely.
-- An admission the cache refuses breaks the chain at its
-- place: the scan stays correct and no claim spans the keys it
-- could not record -- the value and DELETE-record refusals both
-- funnel here. Once the pressure is gone, the next scan
-- re-links the range and repeats are served in full.
g.test_refused_admission_breaks_chain = function(cg)
    cg.server:exec(function(default_cache)
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        box.cfg{vinyl_cache = 16 * 1024}
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        -- The key carries the padding: even the DELETE record,
        -- a key-only copy, is too wide for the slack the brim
        -- leaves under the quota.
        s:create_index('pk', {parts = {{1, 'unsigned'},
                                       {2, 'string'}}})
        local wide = string.rep('y', 1000)
        s:replace{10, wide}
        s:replace{20, wide}
        s:replace{30, wide}
        local pin = helpers.pin_read_view(function() s:get{20, wide} end)
        s:delete{20, wide}
        -- Fill until the first refusal: every resident enters
        -- with the admission credit alone, no walk churns the
        -- set below the brim, and the first refusal proves the
        -- slack under the quota is smaller than one padded
        -- entry -- and so smaller than any wide entry of the
        -- choked scan below.
        local hot = box.schema.space.create('hot', {engine = 'vinyl'})
        hot:create_index('pk')
        local pad = string.rep('x', 200)
        local n = 0
        repeat
            n = n + 1
            hot:replace{n, pad}
            hot:get{n}
        until hot.index.pk:stat().cache.reject > 0
        -- Saturate every resident. The rounds admit nothing:
        -- the one missing key keeps being refused against a
        -- ring the touches keep warm, so nothing churns and
        -- every resident's heat climbs to the cap.
        for _ = 1, 6 do
            for j = 1, n do hot:get{j} end
        end
        -- The choked scan: the values and the guarded DELETE
        -- record are refused, the chain breaks at every refusal,
        -- and the results still stand.
        local reject0 = s.index.pk:stat().cache.reject
        t.assert_equals(s:select{}, {{10, wide}, {30, wide}})
        t.assert_gt(s.index.pk:stat().cache.reject, reject0,
            'the admissions were refused')
        -- The pressure gone, the next scan re-links the range,
        -- and the repeat is served.
        box.cfg{vinyl_cache = default_cache}
        t.assert_equals(s:select{}, {{10, wide}, {30, wide}})
        local stat = s.index.pk:stat()
        local mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s:select{}, {{10, wide}, {30, wide}})
        stat = s.index.pk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the re-linked chain serves the repeat')
        pin.release()
        hot:drop()
    end, {cg.default_cache})
end

g.test_dropped_cache_is_drained = function(cg)
    cg.server:exec(function()
        local quota = 512 * 1024
        -- Start from an empty cache: the memory assertions below
        -- measure this test's spaces alone.
        box.cfg{vinyl_cache = 0}
        box.cfg{vinyl_cache = quota}
        t.assert_equals(box.stat.vinyl().memory.tuple_cache, 0)
        local s = box.schema.create_space('test', {engine = 'vinyl'})
        s:create_index('pk')
        local pad = string.rep('x', 500)
        for i = 1, 200 do s:replace{i, pad} end
        for i = 1, 200 do s:get{i} end
        t.assert_gt(box.stat.vinyl().memory.tuple_cache, 64 * 1024)
        s:drop()
        t.assert_gt(box.stat.vinyl().memory.tuple_cache, 0)
        box.cfg{vinyl_cache = 0}
        t.assert_equals(box.stat.vinyl().memory.tuple_cache, 0)
    end)
end

--
-- 6. Secondary index coherency.
--
-- A write updates a secondary index only when it changes the
-- indexed fields, so a secondary source can yield a superseded
-- row. A result that is not maybe stale is the newest version
-- of its row and is served as is; any other result is resolved
-- against the primary, and the resolved row replaces the stale
-- entry in place; a moved or deleted key is invalidated at
-- prepare. A repeat secondary read is served from the secondary
-- cache alone: the primary index is not consulted at all.
--

-- An update that leaves the indexed fields unchanged does not
-- write the secondary index or invalidate its cache: the cached
-- entry keeps the old tuple. The first read after the update
-- returns the new fields -- the resolution replaces the stale
-- entry -- and the repeat read needs no primary lookup.
g.test_unindexed_update_refreshes_secondary_entry = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10, 'old'}
        s:replace{2, 20, 'x'}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'old'}, {2, 20, 'x'}})
        s:replace{1, 10, 'new'}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'new'}, {2, 20, 'x'}})
        local sk = s.index.sk:stat()
        local pk = s.index.pk:stat()
        local flat = {sk.memory.iterator.lookup,
                      sk.disk.iterator.lookup,
                      pk.lookup,
                      pk.memory.iterator.lookup,
                      pk.disk.iterator.lookup}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'new'}, {2, 20, 'x'}})
        sk = s.index.sk:stat()
        pk = s.index.pk:stat()
        t.assert_equals({sk.memory.iterator.lookup,
                         sk.disk.iterator.lookup,
                         pk.lookup,
                         pk.memory.iterator.lookup,
                         pk.disk.iterator.lookup}, flat,
            'the repeat read is served from the secondary '..
            'cache alone')
    end)
end

-- A secondary entry dies with its primary entry: once the row
-- leaves the primary cache -- here through the invalidation of
-- an update that does not touch the indexed fields -- the
-- secondary entry becomes maybe stale. It is not served, the
-- scan descends past it, and the admission of the resolved
-- fresh row replaces it in place: the repeat scan is served.
g.test_secondary_entry_dies_with_primary = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10, 'old'}
        s:replace{2, 20, 'x'}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'old'}, {2, 20, 'x'}})
        local mem = s.index.sk:stat().memory.iterator.lookup
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'old'}, {2, 20, 'x'}})
        t.assert_equals(s.index.sk:stat().memory.iterator.lookup,
                        mem, 'the warm scan is served')
        -- The unindexed update removes the primary entry alone:
        -- the secondary entry is maybe stale from then on.
        s:replace{1, 10, 'new'}
        mem = s.index.sk:stat().memory.iterator.lookup
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'new'}, {2, 20, 'x'}})
        t.assert_gt(s.index.sk:stat().memory.iterator.lookup, mem,
            'the maybe-stale entry is not served: the scan '..
            'descends')
        -- The fresh row replaced it in place: the repeat is
        -- served from both caches.
        local stat = s.index.sk:stat()
        mem = stat.memory.iterator.lookup
        local disk = stat.disk.iterator.lookup
        t.assert_equals(s.index.sk:select{},
                        {{1, 10, 'new'}, {2, 20, 'x'}})
        stat = s.index.sk:stat()
        t.assert_equals({stat.memory.iterator.lookup,
                         stat.disk.iterator.lookup}, {mem, disk},
            'the replaced entry serves the repeat')
    end)
end

-- A row resident in the primary cache is served from a
-- secondary source as is: the repeat scan and the point read
-- perform no primary lookup, not even into its cache.
g.test_secondary_hit_needs_no_primary_lookup = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {parts = {{2, 'unsigned'}}})
        local expect = {}
        for i = 1, 5 do
            s:replace{i, i * 10}
            table.insert(expect, {i, i * 10})
        end
        -- The warm scan resolves every row through the primary.
        t.assert_equals(s.index.sk:select{}, expect)
        local lookups = s.index.pk:stat().lookup
        t.assert_equals(s.index.sk:select{}, expect)
        t.assert_equals(s.index.sk:get{30}, {3, 30})
        t.assert_equals(s.index.pk:stat().lookup, lookups,
            'a secondary hit needs no primary lookup')
    end)
end

-- A write that moves a row to another secondary key writes the
-- secondary index: invalidation removes the old key's entry at
-- prepare, so no reader ever sees the row under it, and the new
-- key is read from the deeper sources once. The repeat scan
-- needs no primary lookup.
g.test_secondary_key_move_invalidates = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 30}
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {2, 30}})
        s:replace{1, 20}
        t.assert_equals(s.index.sk:select{}, {{1, 20}, {2, 30}})
        t.assert_equals(s.index.sk:select{10}, {})
        local sk = s.index.sk:stat()
        local pk = s.index.pk:stat()
        local flat = {sk.memory.iterator.lookup,
                      sk.disk.iterator.lookup,
                      pk.lookup,
                      pk.memory.iterator.lookup,
                      pk.disk.iterator.lookup}
        t.assert_equals(s.index.sk:select{}, {{1, 20}, {2, 30}})
        sk = s.index.sk:stat()
        pk = s.index.pk:stat()
        t.assert_equals({sk.memory.iterator.lookup,
                         sk.disk.iterator.lookup,
                         pk.lookup,
                         pk.memory.iterator.lookup,
                         pk.disk.iterator.lookup}, flat,
            'the repeat scan is served from the secondary '..
            'cache alone')
    end)
end

-- A delete writes the secondary index: invalidation removes the
-- entry at prepare, and the scan that consumes the DELETE links
-- its neighbors across the dead key -- the DELETE itself serves
-- no reader and is not recorded. The repeat scan needs no
-- primary lookup.
g.test_secondary_delete_serves_absence = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('sk', {unique = false,
                              parts = {{2, 'unsigned'}}})
        s:replace{1, 10}
        s:replace{2, 20}
        s:replace{3, 30}
        t.assert_equals(s.index.sk:select{},
                        {{1, 10}, {2, 20}, {3, 30}})
        s:delete{2}
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {3, 30}})
        local sk = s.index.sk:stat()
        local pk = s.index.pk:stat()
        local flat = {sk.memory.iterator.lookup,
                      sk.disk.iterator.lookup,
                      pk.lookup,
                      pk.memory.iterator.lookup,
                      pk.disk.iterator.lookup}
        t.assert_equals(s.index.sk:select{}, {{1, 10}, {3, 30}})
        sk = s.index.sk:stat()
        pk = s.index.pk:stat()
        t.assert_equals({sk.memory.iterator.lookup,
                         sk.disk.iterator.lookup,
                         pk.lookup,
                         pk.memory.iterator.lookup,
                         pk.disk.iterator.lookup}, flat,
            'the repeat scan is served from the secondary '..
            'cache alone')
    end)
end

-- A multikey update: the removed key is invalidated, the added
-- key is read from the deeper sources once, and a key the update
-- kept becomes fresh however it got there -- rewritten and
-- invalidated, or left stale and replaced by the validation.
-- All entries of one row share its tuple object, and the repeat
-- scan is served from the secondary cache alone.
g.test_multikey_update_stays_coherent = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {parts = {{1, 'unsigned'}}})
        s:create_index('mk', {unique = false,
                              parts = {{field = 2,
                                        type = 'unsigned',
                                        path = '[*]'}}})
        s:replace{1, {10, 20}}
        s:replace{2, {40}}
        t.assert_equals(s.index.mk:select{},
                        {{1, {10, 20}}, {1, {10, 20}}, {2, {40}}})
        s:replace{1, {10, 30}}
        -- Key 20 is gone, key 30 arrived, key 10 serves the new
        -- tuple.
        t.assert_equals(s.index.mk:select{20}, {})
        t.assert_equals(s.index.mk:select{30}, {{1, {10, 30}}})
        t.assert_equals(s.index.mk:select{10}, {{1, {10, 30}}})
        t.assert_equals(s.index.mk:select{},
                        {{1, {10, 30}}, {1, {10, 30}}, {2, {40}}})
        local mk = s.index.mk:stat()
        local pk = s.index.pk:stat()
        local flat = {mk.memory.iterator.lookup,
                      mk.disk.iterator.lookup,
                      pk.lookup,
                      pk.memory.iterator.lookup,
                      pk.disk.iterator.lookup}
        t.assert_equals(s.index.mk:select{},
                        {{1, {10, 30}}, {1, {10, 30}}, {2, {40}}})
        mk = s.index.mk:stat()
        pk = s.index.pk:stat()
        t.assert_equals({mk.memory.iterator.lookup,
                         mk.disk.iterator.lookup,
                         pk.lookup,
                         pk.memory.iterator.lookup,
                         pk.disk.iterator.lookup}, flat,
            'the repeat scan is served from the secondary '..
            'cache alone')
    end)
end

--
-- 7. Restore and cache version.
--
-- A reader that yields mid-scan revalidates its cache position
-- when the cache version moved: an intact frontier is a hop, a
-- dead one a re-seek, an unchanged version costs nothing. Every
-- test runs three flavors -- a forward primary scan, a reverse
-- multikey scan and a reverse multikey EQ -- since direction
-- and duplicate-statement entries bend every corner of the
-- walk.
--

-- Restores that must not disturb the scan: a pause with no
-- cache change (the version is unchanged, the restore is free),
-- then an admission outside the scanned range (the version
-- moves, the restore hops from the frontier over an unchanged
-- gap). The resumed walk stays cache-served either way.
g.test_restore_after_unrelated_admission = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local flavors = {
            {
                name = 'forward',
                setup = function()
                    local s = box.schema.space.create(
                        'fwd', {engine = 'vinyl'})
                    s:create_index('pk')
                    s:replace{5}
                    for i = 1, 6 do s:replace{i * 10} end
                    return s.index.pk, {10}, {iterator = 'GE'},
                           function() s:get{5} end
                end,
            },
            {
                name = 'reverse multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'rmk', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10, 20}}
                    s:replace{2, {30}}
                    s:replace{3, {40, 45}}
                    s:replace{4, {90}}
                    return mk, {50}, {iterator = 'LE'},
                           function() mk:select{90} end
                end,
            },
            {
                name = 'reverse eq multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'req', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10}}
                    s:replace{2, {10, 20}}
                    s:replace{3, {10}}
                    s:replace{4, {70}}
                    return mk, {10}, {iterator = 'REQ'},
                           function() mk:select{70} end
                end,
            },
        }
        for _, fl in ipairs(flavors) do
            local index, key, opts, aux = fl.setup()
            local expect = index:select(key, opts)
            t.assert_ge(#expect, 3, fl.name)
            index:select(key, opts)
            local rd = helpers.stepped_scan(index, key, opts)
            -- The version is unchanged across this pause: the
            -- restore before the second step is free.
            t.assert_equals(rd.step(), expect[1]:totable(), fl.name)
            t.assert_equals(rd.step(), expect[2]:totable(), fl.name)
            -- The admission outside the scanned range moves the
            -- cache version under the paused reader.
            aux()
            -- The resumed walk hops from its frontier and stays
            -- cache-served: no descent to memory or disk.
            local stat = index:stat()
            local flat = {stat.memory.iterator.lookup,
                          stat.disk.iterator.lookup}
            for i = 3, #expect do
                t.assert_equals(rd.step(), expect[i]:totable(),
                                fl.name)
            end
            t.assert_equals(rd.step(), nil, fl.name)
            stat = index:stat()
            t.assert_equals({stat.memory.iterator.lookup,
                             stat.disk.iterator.lookup}, flat,
                fl.name .. ': the resumed scan is cache-served')
            rd.stop()
        end
    end)
end

-- A restore that hops into a gap another reader repopulated:
-- the paused reader stops before an uncached disk-backed tail,
-- a concurrent scan reads and caches it, and the resumed reader
-- is served the tail from the cache without its own descent.
g.test_restore_hops_into_repopulated_gap = function(cg)
    cg.server:exec(function(default_cache)
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local flavors = {
            {
                name = 'forward',
                setup = function()
                    local s = box.schema.space.create(
                        'fwd', {engine = 'vinyl'})
                    s:create_index('pk')
                    for i = 1, 6 do s:replace{i * 10} end
                    return s.index.pk, {10}, {iterator = 'GE'}
                end,
            },
            {
                name = 'reverse multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'rmk', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10, 20}}
                    s:replace{2, {30}}
                    s:replace{3, {40, 45}}
                    return mk, {50}, {iterator = 'LE'}
                end,
            },
            {
                name = 'reverse eq multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'req', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10}}
                    s:replace{2, {10, 20}}
                    s:replace{3, {10}}
                    s:replace{4, {10}}
                    return mk, {10}, {iterator = 'REQ'}
                end,
            },
        }
        for _, fl in ipairs(flavors) do
            local index, key, opts = fl.setup()
            box.snapshot()
            local expect = index:select(key, opts)
            t.assert_ge(#expect, 4, fl.name)
            -- Re-warm the head only: the first scan above built
            -- the full chain, so flush and cache the head alone.
            box.cfg{vinyl_cache = 0}
            box.cfg{vinyl_cache = default_cache}
            local head_opts = {iterator = opts.iterator, limit = 2}
            t.assert_equals(#index:select(key, head_opts), 2)
            local rd = helpers.stepped_scan(index, key, opts)
            t.assert_equals(rd.step(), expect[1]:totable(), fl.name)
            t.assert_equals(rd.step(), expect[2]:totable(), fl.name)
            -- The concurrent scan pays the disk for the tail and
            -- caches it, moving the version under the reader.
            t.assert_equals(index:select(key, opts), expect,
                            fl.name)
            -- The resumed reader is served the tail: no disk.
            local disk = index:stat().disk.iterator.lookup
            for i = 3, #expect do
                t.assert_equals(rd.step(), expect[i]:totable(),
                                fl.name)
            end
            t.assert_equals(rd.step(), nil, fl.name)
            t.assert_equals(index:stat().disk.iterator.lookup,
                            disk,
                fl.name .. ': the repopulated tail is cache-served')
            rd.stop()
        end
    end, {cg.default_cache})
end

-- A restore whose position is gone: the cache is purged while
-- the reader is paused, so the frontier is dead and the restore
-- re-seeks. The scan misses and duplicates nothing, and the
-- cache is healthy afterwards: a re-warmed scan is served.
g.test_restore_reseeks_after_purge = function(cg)
    cg.server:exec(function(default_cache)
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local flavors = {
            {
                name = 'forward',
                setup = function()
                    local s = box.schema.space.create(
                        'fwd', {engine = 'vinyl'})
                    s:create_index('pk')
                    for i = 1, 6 do s:replace{i * 10} end
                    return s.index.pk, {10}, {iterator = 'GE'}
                end,
            },
            {
                name = 'reverse multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'rmk', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10, 20}}
                    s:replace{2, {30}}
                    s:replace{3, {40, 45}}
                    return mk, {50}, {iterator = 'LE'}
                end,
            },
            {
                name = 'reverse eq multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'req', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10}}
                    s:replace{2, {10, 20}}
                    s:replace{3, {10}}
                    return mk, {10}, {iterator = 'REQ'}
                end,
            },
        }
        for _, fl in ipairs(flavors) do
            local index, key, opts = fl.setup()
            local expect = index:select(key, opts)
            t.assert_ge(#expect, 3, fl.name)
            local rd = helpers.stepped_scan(index, key, opts)
            t.assert_equals(rd.step(), expect[1]:totable(), fl.name)
            t.assert_equals(rd.step(), expect[2]:totable(), fl.name)
            box.cfg{vinyl_cache = 0}
            box.cfg{vinyl_cache = default_cache}
            for i = 3, #expect do
                t.assert_equals(rd.step(), expect[i]:totable(),
                                fl.name)
            end
            t.assert_equals(rd.step(), nil, fl.name)
            rd.stop()
            -- The second reader crosses the purge while the
            -- cache stays disabled: the scan is fed from the
            -- deeper sources alone and records nothing.
            rd = helpers.stepped_scan(index, key, opts)
            t.assert_equals(rd.step(), expect[1]:totable(), fl.name)
            box.cfg{vinyl_cache = 0}
            for i = 2, #expect do
                t.assert_equals(rd.step(), expect[i]:totable(),
                                fl.name)
            end
            t.assert_equals(rd.step(), nil, fl.name)
            rd.stop()
            box.cfg{vinyl_cache = default_cache}
            t.assert_equals(index:select(key, opts), expect,
                            fl.name)
            local stat = index:stat()
            local flat = {stat.memory.iterator.lookup,
                          stat.disk.iterator.lookup}
            t.assert_equals(index:select(key, opts), expect,
                            fl.name)
            stat = index:stat()
            t.assert_equals({stat.memory.iterator.lookup,
                             stat.disk.iterator.lookup}, flat,
                fl.name .. ': the re-warmed scan is served')
        end
    end, {cg.default_cache})
end

-- A restore across a memory-level rotation: a row committed
-- ahead of the paused reader, then a checkpoint rotating the
-- memory level under it. The rebuilt sources skip to the
-- reader's position -- possibly already past it -- and the scan
-- surfaces the fresh row exactly once.
g.test_restore_across_mem_rotation = function(cg)
    cg.server:exec(function()
        local helpers = require('test.vinyl-luatest.tuple_cache_helpers')
        local flavors = {
            {
                name = 'forward',
                setup = function()
                    local s = box.schema.space.create(
                        'fwd', {engine = 'vinyl'})
                    s:create_index('pk')
                    for i = 1, 6 do s:replace{i * 10} end
                    return s.index.pk, {10}, {iterator = 'GE'},
                           function() s:replace{35} end
                end,
            },
            {
                name = 'reverse multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'rmk', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10, 20}}
                    s:replace{2, {30}}
                    s:replace{3, {40, 45}}
                    return mk, {50}, {iterator = 'LE'},
                           function() s:replace{5, {25}} end
                end,
            },
            {
                name = 'reverse eq multikey',
                setup = function()
                    local s = box.schema.space.create(
                        'req', {engine = 'vinyl'})
                    s:create_index('pk')
                    local mk = s:create_index('mk', {
                        unique = false,
                        parts = {{field = 2, type = 'unsigned',
                                  path = '[*]'}}})
                    s:replace{1, {10}}
                    s:replace{2, {10, 20}}
                    s:replace{3, {10}}
                    return mk, {10}, {iterator = 'REQ'},
                           function() s:replace{0, {10}} end
                end,
            },
        }
        for _, fl in ipairs(flavors) do
            local index, key, opts, write = fl.setup()
            -- Warm the head only: past it the cache source is
            -- exhausted, and the source rebuild finds it already
            -- positioned at its end.
            index:select(key, {iterator = opts.iterator, limit = 2})
            local rd = helpers.stepped_scan(index, key, opts)
            local got = {}
            table.insert(got, rd.step())
            table.insert(got, rd.step())
            -- The write lands ahead of the reader's position, in
            -- a range it has not read: the reader must see it.
            write()
            box.snapshot()
            while true do
                local row = rd.step()
                if row == nil then
                    break
                end
                table.insert(got, row)
            end
            rd.stop()
            local expect = {}
            for _, tuple in ipairs(index:select(key, opts)) do
                table.insert(expect, tuple:totable())
            end
            t.assert_equals(got, expect,
                fl.name .. ': the scan sees the fresh row once')
        end
    end)
end

--
-- 8. Slice bounds.
--
-- The total key order at slice bounds: coverage of the reverse
-- seek clamp the cache's bound keys made necessary.
--

--
-- A reverse scan with a partial key must not return tuples
-- matching the key prefix: LT {1} excludes every {1, *}. The result
-- must hold when the range tree is split at a boundary extending
-- the searched prefix, so that a slice's exclusive upper bound
-- ties with the search key on its common parts.
--
g.test_reverse_partial_scan_after_split = function(cg)
    cg.server:exec(function()
        local s = box.schema.space.create('test', {engine = 'vinyl'})
        s:create_index('pk', {
            parts = {{1, 'unsigned'}, {2, 'unsigned'}},
            range_size = 4 * 1024,
            page_size = 1024,
            run_count_per_level = 1,
        })
        local pad = string.rep('x', 256)
        s:replace{0, 1, pad}
        for _ = 1, 6 do
            for i = 1, 500 do s:replace{1, i, pad} end
            box.snapshot()
        end
        t.helpers.retrying({timeout = 120}, function()
            t.assert_ge(s.index.pk:stat().range_count, 2)
        end)
        t.assert_equals(s:select({1}, {iterator = 'LT'}), {s:get{0, 1}})
        t.assert_equals(s:select({0}, {iterator = 'GT'}),
                        s:select({1}, {iterator = 'GE'}))
        s:drop()
    end)
end
