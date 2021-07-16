fiber = require('fiber')
test_run = require('test_run').new()

-- https://github.com/tarantool/tarantool/issues/6147

s = box.schema.create_space('test')
s:format{{'id1'},{'id2'},{'name'}}
_ = s:create_index('pk', {parts={{1, 'unsigned'}, {2, 'unsigned'}}})
N = 16
for i = 1,N do s:replace{0, i, tostring(i)} end
fib = fiber.create(function() for k,v in s:pairs{0} do fiber.sleep(0.001) N = N - 1 end end)
fib:set_joinable(true)
_ = fiber.create(function() s:format{} end)
fib:join()
assert(N == 0)

s:drop()
