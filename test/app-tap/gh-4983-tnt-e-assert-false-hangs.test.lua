#!/usr/bin/env tarantool

local fiber = require('fiber')
local clock = require('clock')
local ffi = require('ffi')
local fio = require('fio')
local errno = require('errno')
local tap = require('tap')
local test = tap.test('gh-4983-tnt-e-assert-false-hangs')

--
-- gh-4983: tarantool -e 'assert(false)' hangs
--

--
-- For process_is_alive.
--
ffi.cdef([[
    int kill(pid_t pid, int sig);
]])

--
--  Verify whether a process is alive.
--
local function process_is_alive(pid)
    local rc = ffi.C.kill(pid, 0)
    return rc == 0 or errno() ~= errno.ESRCH
end

--
--  Return true if process completed before timeout.
--  Return false otherwise.
--
local function wait_process_completion(pid, timeout)
    local start_time = clock.monotonic()
    local process_completed = false
    while clock.monotonic() - start_time < timeout do
        if not process_is_alive(pid) then
            process_completed = true
            break
        end
        clock.monotonic()
    end
    return process_completed
end

--
-- Open file on reading with timeout.
--
local function open_with_timeout(filename, timeout)
    local fh
    local start_time = clock.monotonic()
    while not fh and clock.monotonic() - start_time < timeout do
        fh = fio.open(filename, {'O_RDONLY'})
    end
    return fh
end

--
-- Try to read from file with timeout
-- with interval.
--
local function read_with_timeout(fh, timeout, interval)
    local data = ''
    local start_time = clock.monotonic()
    while #data == 0 and clock.monotonic() - start_time < timeout do
        data = fh:read()
        if #data == 0 then fiber.sleep(interval) end
    end
    return data
end

local TARANTOOL_PATH = arg[-1]
local output_file = fio.abspath('out.txt')
local line = ('%s -e "assert(false)" > %s 2>&1 & echo $!'):format(TARANTOOL_PATH, output_file)
local process_waiting_timeout = 2.0
local file_read_timeout = 2.0
local file_read_interval = 0.2
local file_open_timeout = 2.0

test:plan(2)

local pid = tonumber(io.popen(line):read("*line"))
assert(pid, "pid of proccess can't be recieved")

local process_completed = wait_process_completion(pid, process_waiting_timeout)
test:ok(process_completed, ('tarantool process with pid = %d completed'):format(pid))

-- Kill process if hangs.
if not process_completed then ffi.C.kill(pid, 9) end

local fh = open_with_timeout(output_file, file_open_timeout)
assert(fh, 'error while opening ' .. output_file)

local data = read_with_timeout(fh, file_read_timeout, file_read_interval)
test:like(data, "assertion failed", "assertion failure is displayed")

fh:close()
test:check()
