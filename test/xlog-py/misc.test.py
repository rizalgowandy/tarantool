from __future__ import print_function

import os
import yaml

from os.path import abspath

# cleanup server.vardir
server.stop()
server.deploy()
lsn = int(yaml.safe_load(server.admin("box.info.lsn", silent=True))[0])
server.stop()

data_path = os.path.join(server.vardir, server.name)

print("")
print("# xlog file must exist after inserts.")
print("")
filename = str(lsn).zfill(20) + ".xlog"
wal = os.path.join(data_path, filename)

server.start()

server.admin("space = box.schema.space.create('tweedledum')")
if os.access(wal, os.F_OK):
  print(".xlog exists")

server.admin("index = space:create_index('primary', { type = 'hash' })")

server.stop()
lsn += 2

print("")
print("# a new xlog must be opened after regular termination.")
print("")
filename = str(lsn).zfill(20) + ".xlog"
server.start()

wal = os.path.join(data_path, filename)

server.admin("box.space.tweedledum:insert{3, 'third tuple'}")

if os.access(wal, os.F_OK):
  print("a new .xlog exists")

server.stop()

if os.access(wal, os.F_OK):
  print(".xlog stays around after shutdown")
lsn += 1

print("")
print("# An xlog file with one record during recovery.")
print("")
server.start()
filename = str(lsn).zfill(20) + ".xlog"
wal = os.path.join(data_path, filename)
server.admin("box.space.tweedledum:insert{4, 'fourth tuple'}")
server.admin("box.space.tweedledum:insert{5, 'Unfinished record'}")
pid = int(yaml.safe_load(server.admin("require('tarantool').pid()", silent=True))[0])
from signal import SIGKILL
if pid > 0:
    os.kill(pid, SIGKILL)
server.stop()

if os.access(wal, os.F_OK):
    print(".xlog exists after kill -9")
    # Remove last byte from xlog
    f = open(wal, "a")
    size = f.tell()
    f.truncate(size - 1)
    f.close()

server.start()

if os.access(wal, os.F_OK):
  print("corrupt .xlog exists after start")
server.stop()
lsn += 1

server.start()
orig_lsn = int(yaml.safe_load(admin("box.info.lsn", silent=True))[0])

# create .snap.inprogress
admin("box.snapshot()")
admin("box.space._schema:insert({'test', 'test'})")
admin("box.snapshot()")
lsn = int(yaml.safe_load(admin("box.info.lsn", silent=True))[0])
snapshot = str(lsn).zfill(20) + ".snap"
snapshot = os.path.join(data_path, snapshot)
server.stop()
os.rename(snapshot, snapshot + ".inprogress")
# remove .xlogs and .vylog
for f in os.listdir(data_path):
    if f.endswith((".xlog", ".vylog")):
        os.remove(os.path.join(data_path, f))

# check that .snap.inprogress is ignored during scan
server.start()
lsn = int(yaml.safe_load(admin("box.info.lsn", silent=True))[0])
if lsn == orig_lsn:
    print(".snap.inprogress is ignored")
