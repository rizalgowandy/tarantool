env = require('test_run')
test_run = env.new()
test_run:cmd('create server test with script="engine/small_alloc_factor.lua"')
test_run:cmd('start server test with args="none"')
test_run:cmd('switch test')
s = box.schema.space.create('test')
_ = s:create_index('test')
function str(i) return string.rep('a', math.floor(256 * math.pow(1.03, i))) end
for i=1,276 do s:replace{i+200, str(i)} end
for i=1,274 do s:delete{i+200} end
collectgarbage('collect')
s:replace{200+277, str(275)}
s:delete{200+275}
collectgarbage('collect')
s:delete{200+276}
collectgarbage('collect')
test_run:cmd('switch default')
test_run:cmd('stop server test')
test_run:cmd('cleanup server test')
test_run:cmd('delete server test')
