--# create server replica with configuration='replication/cfg/replica.cfg', rpl_master=default
--# start server replica
--# set connection default
box.schema.user.grant('guest', 'read,write,execute', 'universe')
-- Wait until the grant reaches the replica
--# set connection replica
while box.space['_priv']:len() < 1 do box.fiber.sleep(0.01) end
--# setopt delimiter ';'
--# set connection default, replica
do
    begin_lsn = -1
    function _set_pri_lsn(_lsn)
        begin_lsn = _lsn
    end
    function _print_lsn()
        return (box.info.lsn - begin_lsn + 1)
    end
    function _insert(_begin, _end, msg)
        local a = {}
        for i = _begin, _end do
            table.insert(a, box.space[0]:insert{i, msg..' - '..i})
        end
        return unpack(a)
    end
    function _select(_begin, _end)
        local a = {}
        while box.info.lsn < begin_lsn + _end + 2 do
            box.fiber.sleep(0.001)
        end
        for i = _begin, _end do
            table.insert(a, box.space[0]:get{i})
        end
        return unpack(a)
    end
end;
--# setopt delimiter ''
--# set connection default
--# set variable replica_port to 'replica.primary_port'

-- set begin lsn on master and replica.
begin_lsn = box.info.lsn
a = box.net.box.new('127.0.0.1', replica_port)
a:call('_set_pri_lsn', box.info.lsn)
a:close()

s = box.schema.create_space('tweedledum', {id = 0});
s:create_index('primary', {type = 'hash'})
_insert(1, 10, 'master')
_select(1, 10)
--# set connection replica
_select(1, 10)

--# set connection default
-- Master LSN:
_print_lsn()

--# set connection replica
-- Replica LSN:
_print_lsn()

-----------------------------
--  Master LSN > Replica LSN
-----------------------------
--------------------
-- Replica to Master
--------------------
--# reconfigure server replica with configuration 'replication/cfg/replica_to_master.cfg', rpl_master=None
--# set connection default
_insert(11, 20, 'master')
_select(11, 20)
--# set connection replica
_insert (11, 15, 'replica')
_select (11, 15)

--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

-------------------
-- rollback Replica
-------------------
--# reconfigure server replica with configuration='replication/cfg/replica.cfg', rpl_master=default
_select(11, 20)
--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

------------------------------
--  Master LSN == Replica LSN
------------------------------
--------------------
-- Replica to Master
--------------------
--# reconfigure server replica with configuration='replication/cfg/replica_to_master.cfg', rpl_master=None
--# set connection default
_insert(21, 30, 'master')
_select(21, 30)
--# set connection replica
_insert(21, 30, 'replica')
_select(21, 30)

--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

-------------------
-- rollback Replica
-------------------
--# reconfigure server replica with configuration='replication/cfg/replica.cfg', rpl_master=default
_select(21, 30)

--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

-----------------------------
--  Master LSN < Replica LSN
-----------------------------
--------------------
-- Replica to Master
--------------------
--# reconfigure server replica with configuration='replication/cfg/replica_to_master.cfg', rpl_master=None
--# set connection default
_insert(31, 40, 'master')
_select(31, 40)
--# set connection replica
_insert(31, 50, 'replica')
_select(31, 50)

--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

-------------------
-- rollback Replica
-------------------
--# reconfigure server replica with configuration='replication/cfg/replica.cfg', rpl_master=default
_select(31, 50)
--# set connection default
_insert(41, 60, 'master')
--# set connection replica
_select(41, 60)

--# set connection default
-- Master LSN:
_print_lsn()
--# set connection replica
-- Replica LSN:
_print_lsn()

-- Test that a replica replies with master connection URL on update request
--# push filter '127.0.0.1:.*' to '127.0.0.1:<port>'
box.space[0]:insert{0, 'replica is RO'}
--# clear filter
--# stop server replica
--# cleanup server replica
--# set connection default
box.space[0]:drop()
box.schema.user.revoke('guest', 'read,write,execute', 'universe')
