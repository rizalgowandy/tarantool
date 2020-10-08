test_run = require('test_run').new()

--
-- gh-3228: Check the error message in the case of a __serialize
-- function generating infinite recursion.
--
setmetatable({}, {__serialize = function(a) return a end})

--
--Check that __eq metamethod is ignored.
--
local table = setmetatable({}, {__eq = function(a, b) error('__eq is called') end})
setmetatable(table, {__serialize = function(a) return a end})
