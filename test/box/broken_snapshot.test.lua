
-- write data recover from latest snapshot
env = require('test_run')
test_run = env.new()

test_run:cmd("restart server default")
fio = require 'fio'
test_run:cmd("setopt delimiter ';'")
function get_snap_file ()
    local snapfile = nil	
    local directory = fio.pathjoin(fio.cwd(), 'broken_snapshot')
    for files in io.popen(string.format("ls %s", directory)):lines() do
        local snaps = string.find(files, "snap")
        if (snaps ~= nil) then
            local snap = string.find(files, "%n")
	    if (snap ~= nil) then
                snapfile = string.format("%s/%s", directory, files)
            end
        end
    end
    return snapfile
end;
test_run:cmd("setopt delimiter ''");


test_run:cmd("create server test with script='box/broken_snapshot.lua'")
test_run:cmd("start server test")
test_run:cmd("switch test")
space = box.schema.space.create('test', { engine = "memtx" })
space:format({ {name = 'id', type = 'unsigned'}, {name = 'year', type = 'unsigned'} })
index = space:create_index('primary', { parts = {'id'} })

for key = 1, 10000 do space:insert({key, key + 1000}) end
box.snapshot()

test_run:cmd("switch default")
snapfile = get_snap_file()
file = io.open(snapfile, "r")
size = file:seek("end")
if size > 30000 then size = 30000 end

-- save last snapshot
os.execute(string.format('cp %s %s.save', snapfile, snapfile))
-- write garbage at the end of file
file = io.open(snapfile, "ab")
for i = 1, 1000, 1 do file:write(math.random(0,255)) end
test_run:cmd("switch test")
test_run:cmd("restart server test with script='box/broken_snapshot.lua'")
test_run:cmd("setopt delimiter ';'")
-- check that all data valid
val = box.space.test:select()
for i = 1, 10000, 1 do 
    if (val[i] == nil) then
    	os.exit()
    end
end;
test_run:cmd("setopt delimiter ''");

test_run:cmd("switch default")
-- restore snapshot
os.execute(string.format('cp %s.save %s', snapfile, snapfile))
-- truncate
os.execute(string.format('dd if=%s.save of=%s bs=%d count=1', snapfile, snapfile, size))

test_run:cmd("switch test")
test_run:cmd("restart server test with script='box/broken_snapshot.lua'")
-- check than some data valid
test_run:cmd("setopt delimiter ';'")
val = box.space.test:select();
for i = 1, 1000, 1 do
    if (val[i] == nil) then
        os.exit()
    end
end;
test_run:cmd("setopt delimiter ''");

test_run:cmd("switch default")
-- restore snapshot
os.execute(string.format('cp %s.save %s', snapfile, snapfile))
-- write garbage at the middle of file
file = io.open(snapfile, "r+b")
file:seek("set", size)
for i = 1, 1000, 1 do file:write(math.random(0,255)) end
test_run:cmd("switch test")
test_run:cmd("restart server test with script='box/broken_snapshot.lua'")
test_run:cmd("setopt delimiter ';'")
-- check that some data valid
val = box.space.test:select();
for i = 1, 1000, 1 do
    if (val[i] == nil) then
        os.exit()
    end
end;
for i = 9000, 10000, 1 do
    if (val[i] == nil) then
	    os.exit()
    end
end;
test_run:cmd("setopt delimiter ''");

test_run:cmd("switch default")
-- restore snapshot
os.execute(string.format('cp %s.save %s', snapfile, snapfile))
os.execute(string.format('rm %s.save', snapfile))
-- write big garbage at the middle of file, check that start data valid
file = io.open(snapfile, "r+b")
file:seek("set", size / 2 + 8000)
for i = 1, 10000, 1 do file:write(math.random(0,255)) end
test_run:cmd("switch test")
test_run:cmd("restart server test with script='box/broken_snapshot.lua'")
test_run:cmd("setopt delimiter ';'")
-- check that some data valid
val = box.space.test:select();
for i = 1, 1000, 1 do
    if (val[i] == nil) then
        os.exit()
    end
end;
test_run:cmd("setopt delimiter ''");
test_run:cmd("switch default")
test_run:cmd("stop server test")
test_run:cmd("cleanup server test")
test_run:cmd("delete server test")
