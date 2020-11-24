test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

box.schema.user.grant('guest', 'replication')

-- Test syntax error
box.cfg{replication_synchro_quorum = "aaa"}

-- Make sure we were configured in a proper way
box.cfg { replication_synchro_quorum = "N/2+1", replication_synchro_timeout = 1000 }
match = 'set \'replication_synchro_quorum\' configuration option to \"N\\/2%+1'
test_run:grep_log("default", match) ~= nil

test_run:cmd('create server replica1 with rpl_master=default,\
              script="replication/replica-quorum-1.lua"')
test_run:cmd('start server replica1 with wait=True, wait_load=True')
test_run:wait_lsn('replica1', 'default')

-- 1 replica -> replication_synchro_quorum = 1/2 + 1 = 1
match = 'update replication_synchro_quorum = 1'
test_run:grep_log("default", match) ~= nil

test_run:cmd('create server replica2 with rpl_master=default,\
              script="replication/replica-quorum-2.lua"')
test_run:cmd('start server replica2 with wait=True, wait_load=True')
test_run:wait_lsn('replica2', 'default')

-- 2 replicas -> replication_synchro_quorum = 2/2 + 1 = 2
match = 'update replication_synchro_quorum = 2'
test_run:grep_log("default", match) ~= nil

test_run:cmd('create server replica3 with rpl_master=default,\
              script="replication/replica-quorum-3.lua"')
test_run:cmd('start server replica3 with wait=True, wait_load=True')
test_run:wait_lsn('replica3', 'default')

-- 3 replicas -> replication_synchro_quorum = 3/2 + 1 = 2
match = 'update replication_synchro_quorum = 2'
test_run:grep_log("default", match) ~= nil

test_run:cmd('create server replica4 with rpl_master=default,\
              script="replication/replica-quorum-4.lua"')
test_run:cmd('start server replica4 with wait=True, wait_load=True')
test_run:wait_lsn('replica4', 'default')

-- 4 replicas -> replication_synchro_quorum = 4/2 + 1 = 3
match = 'update replication_synchro_quorum = 3'
test_run:grep_log("default", match) ~= nil

test_run:cmd('create server replica5 with rpl_master=default,\
              script="replication/replica-quorum-5.lua"')
test_run:cmd('start server replica5 with wait=True, wait_load=True')

test_run:cmd('create server replica6 with rpl_master=default,\
              script="replication/replica-quorum-6.lua"')
test_run:cmd('start server replica6 with wait=True, wait_load=True')

test_run:wait_lsn('replica5', 'default')
test_run:wait_lsn('replica6', 'default')

-- 6 replicas -> replication_synchro_quorum = 6/2 + 1 = 4
match = 'update replication_synchro_quorum = 4'
test_run:grep_log("default", match) ~= nil

test_run:cmd('stop server replica1')
test_run:cmd('delete server replica1')

test_run:cmd('stop server replica2')
test_run:cmd('delete server replica2')

test_run:cmd('stop server replica3')
test_run:cmd('delete server replica3')

test_run:cmd('stop server replica4')
test_run:cmd('delete server replica4')

test_run:cmd('stop server replica5')
test_run:cmd('delete server replica5')

test_run:cmd('stop server replica6')
test_run:cmd('delete server replica6')

box.schema.user.revoke('guest', 'replication')
