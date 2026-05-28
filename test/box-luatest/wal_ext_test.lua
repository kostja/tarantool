local server = require('luatest.server')
local t = require('luatest')
local replica_set = require('luatest.replica_set')

local g = t.group()

g.test_wal_ext_dynamic = function()
    -- Boot with wal_ext = nil.
    g.server = server:new({ alias = 'master', box_cfg = { wal_ext = nil } })
    g.server:start()

    g.server:exec(function()
        local fio = require('fio')
        local xlog = require('xlog').pairs

        -- Create a space *before* enabling wal_ext; the dynamic
        -- handler is responsible for re-binding its cached
        -- wal_ext pointer.
        local s = box.schema.space.create('test')
        s:create_index('pk')

        -- Pre-snapshot to anchor what follows in the xlog.
        box.snapshot()

        local function last_checkpoint_signature()
            local ck = box.info.gc().checkpoints
            return ck[#ck].signature
        end
        local sig_before = last_checkpoint_signature()
        -- Write something so the auto-snapshot has new state to
        -- record (otherwise gc_checkpoint() may no-op).
        s:replace{0, 'seed'}

        -- Enable wal_ext dynamically. false -> true must
        -- succeed and must auto-trigger a checkpoint.
        box.cfg { wal_ext = { new_old = true } }
        t.assert(last_checkpoint_signature() > sig_before,
                 'enabling wal_ext must auto-snapshot')

        -- Write into the pre-existing space and verify the
        -- WAL row carries IPROTO_OLD_TUPLE on UPDATE.
        s:replace{1, 'a'}
        s:update({1}, {{'=', 2, 'b'}})

        local last_xlog = fio.glob(
            fio.pathjoin(box.cfg.wal_dir, '*.xlog'))
        table.sort(last_xlog)
        last_xlog = last_xlog[#last_xlog]
        local rows = {}
        for _, v in xlog(last_xlog) do
            table.insert(rows, v)
        end
        local update_row
        for _, v in ipairs(rows) do
            if v.HEADER.type == 'UPDATE' and
               v.BODY.space_id == s.id then
                update_row = v
                break
            end
        end
        t.assert(update_row ~= nil, 'no UPDATE row in xlog')
        t.assert(update_row.BODY.old_tuple ~= nil,
                 'UPDATE row must carry old_tuple after wal_ext flip')

        -- Disabling: also dynamic, but no auto-snapshot.
        sig_before = last_checkpoint_signature()
        box.cfg { wal_ext = { new_old = false } }
        t.assert(last_checkpoint_signature() == sig_before,
                 'disabling wal_ext must NOT auto-snapshot')

        -- Idempotent re-set: no snapshot.
        box.cfg { wal_ext = { new_old = false } }
        t.assert(last_checkpoint_signature() == sig_before,
                 'unchanged wal_ext must NOT auto-snapshot')

        s:drop()
    end)

    g.server:stop()
end

g.test_new_old_extension_enabled = function()
    -- enable new_old extension
    g.server = server:new({
        alias = 'master',
        box_cfg = { wal_ext = { new_old = true } },
    })
    g.server:start()

    g.server:exec(function()
        local fio = require('fio')
        local xlog = require('xlog').pairs

        local function read_xlog(file)
            local val = {}
            for _, v in xlog(file) do
                table.insert(val, v)
            end
            return val
        end

        box.schema.space.create('test'):create_index('pk')

        -- generate a new xlog
        box.snapshot()
        local lsn = box.info.lsn

        box.space.test:insert({ 1, "1" })
        box.space.test:insert({ 2, "2" })
        box.space.test:update(2, { { '=', 2, '3' } })
        box.space.test:delete(1)

        -- open a new xlog
        box.snapshot()

        -- read a previous one xlog, assert new and old tuple information
        local log_path = fio.pathjoin(
                box.cfg.wal_dir,
                string.format('%020d.xlog', lsn)
        )
        local data = read_xlog(log_path)
        t.assert(data[1].HEADER.type == 'INSERT'
                and table.equals(data[1].BODY.new_tuple:totable(), { 1, "1" })
                and data[1].BODY.old_tuple == nil
        )
        t.assert(data[2].HEADER.type == 'INSERT'
                and table.equals(data[2].BODY.new_tuple:totable(), { 2, "2" })
                and data[2].BODY.old_tuple == nil
        )
        t.assert(data[3].HEADER.type == 'UPDATE'
                and table.equals(data[3].BODY.new_tuple:totable(), { 2, "3" })
                and table.equals(data[3].BODY.old_tuple:totable(), { 2, "2" })
        )
        t.assert(data[4].HEADER.type == 'DELETE'
                and data[4].BODY.new_tuple == nil
                and table.equals(data[4].BODY.old_tuple:totable(), { 1, "1" })
        )

        box.space.test:drop()
    end)

    g.server:stop()
end

g.test_new_old_extension_disabled = function()
    -- disable new_old extension
    g.server = server:new({
        alias = 'master',
        box_cfg = { wal_ext = { new_old = false } },
    })
    g.server:start()

    g.server:exec(function()
        local fio = require('fio')
        local xlog = require('xlog').pairs

        local function read_xlog(file)
            local val = {}
            for _, v in xlog(file) do
                table.insert(val, v)
            end
            return val
        end

        -- generate a new xlog
        box.snapshot()
        local lsn = box.info.lsn
        box.schema.space.create('test'):create_index('pk')

        box.space.test:insert({ 3, "3" })
        box.space.test:update(3, { { '=', 2, '4' } })
        box.space.test:delete(3)

        -- open a new xlog
        box.snapshot()

        -- read a previous one xlog, assert new and old tuple information
        -- doesn't exists
        local log_path = fio.pathjoin(
                box.cfg.wal_dir,
                string.format('%020d.xlog', lsn)
        )
        local data = read_xlog(log_path)
        t.assert(data[3].HEADER.type == 'INSERT'
                and data[3].BODY.new_tuple == nil
                and data[3].BODY.old_tuple == nil
        )
        t.assert(data[4].HEADER.type == 'UPDATE'
                and data[4].BODY.new_tuple == nil
                and data[4].BODY.old_tuple == nil
        )
        t.assert(data[5].HEADER.type == 'DELETE'
                and data[5].BODY.new_tuple == nil
                and data[5].BODY.old_tuple == nil
        )

        box.space.test:drop()
    end)

    g.server:stop()
end

g.test_new_old_extension_replicated = function()
    g.rs = replica_set:new()
    g.master_box_cfg = {
        replication_timeout = 0.1,
        replication_connect_timeout = 10,
        replication_sync_lag = 0.01,
        replication_connect_quorum = 3,
        replication = {
            server.build_listen_uri('master', g.rs.id),
            server.build_listen_uri('replica', g.rs.id),
        },
        -- enable new_old extension only on master
        wal_ext = { new_old = true },
    }
    g.replica_box_cfg = {
        replication_timeout = 0.1,
        replication_connect_timeout = 10,
        replication_sync_lag = 0.01,
        replication_connect_quorum = 3,
        replication = {
            server.build_listen_uri('master', g.rs.id),
            server.build_listen_uri('replica', g.rs.id),
        },
    }

    g.rs:build_and_add_server({
        alias = 'master',
        box_cfg = g.master_box_cfg
    })
    g.rs:build_and_add_server({
        alias = 'replica',
        box_cfg = g.replica_box_cfg
    })
    g.rs:start()

    local lsn = g.rs:get_server('master'):exec(function()
        local lsn = box.info.lsn

        box.schema.space.create('test'):create_index('pk')

        box.space.test:insert({ 1, "1" })
        box.space.test:insert({ 2, "2" })
        box.space.test:update(2, { { '=', 2, '3' } })
        box.space.test:delete(1)

        return lsn
    end)

        local function check_vclock_synchronized()
                local function get_vclock(node_name)
                        return g.rs:get_server(node_name)
                                :exec(function() return box.info.vclock end)
                end
                local master_vclock = get_vclock("master")
                local replica_vclock = get_vclock("replica")

                t.assert_equals(
                        master_vclock,
                        replica_vclock,
                        'Vclocks are not synchronized'
                )
        end
        t.helpers.retrying(
                {timeout = 2, delay = 0.1},
                check_vclock_synchronized
        )

    g.rs:get_server('replica'):exec(function(lsn)
        local fio = require('fio')
        local xlog = require('xlog').pairs

        local function read_xlog(file)
            local val = {}
            for _, v in xlog(file) do
                table.insert(val, v)
            end
            return val
        end

        -- read a previous one xlog, assert new and old tuple information
        local log_path = fio.pathjoin(
                box.cfg.wal_dir,
                string.format('%020d.xlog', lsn)
        )

        local data = read_xlog(log_path)
        local xlogs_cnt = #data

        local i = xlogs_cnt - 3
        t.assert(data[i].HEADER.type == 'INSERT'
                and table.equals(data[i].BODY.new_tuple:totable(), { 1, "1" })
                and data[i].BODY.old_tuple == nil
        )
        local i = xlogs_cnt - 2
        t.assert(data[i].HEADER.type == 'INSERT'
                and table.equals(data[i].BODY.new_tuple:totable(), { 2, "2" })
                and data[i].BODY.old_tuple == nil
        )
        local i = xlogs_cnt - 1
        t.assert(data[i].HEADER.type == 'UPDATE'
                and table.equals(data[i].BODY.new_tuple:totable(), { 2, "3" })
                and table.equals(data[i].BODY.old_tuple:totable(), { 2, "2" })
        )
        local i = xlogs_cnt
        t.assert(data[i].HEADER.type == 'DELETE'
                and data[i].BODY.new_tuple == nil
                and table.equals(data[i].BODY.old_tuple:totable(), { 1, "1" })
        )
    end, { lsn })

    g.rs:stop()
end
