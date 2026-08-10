-- test_wait.lua
-- The yield-aware layer: queue.wait, on_full = "block", and the wait-set
-- protocol of doc/Messaging.md 8.3.
--
-- Run with --task (see test/run_tests.sh), because parking needs a host that
-- resumes. That is the point of the flag rather than a quirk of the test: the
-- default entry keeps stock Lua semantics, so a program can only park where a
-- host put it on a thread.
--
-- The most important assertion in this file is the one that a wait inside
-- 'pcall' SUCCEEDS. The first draft of 8.4 listed pcall as non-yieldable and
-- had this milestone assert the opposite; yielding across a protected call has
-- been legal since Lua 5.2, and enforcing that restriction would have removed
-- capability standard Lua has.

local pass, fail = 0, 0

local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end

local function eq(actual, expected, what)
    if actual == expected then pass = pass + 1
    else
        fail = fail + 1
        print(string.format("[FAIL] %s\n  expected %s\n  actual   %s",
              what, tostring(expected), tostring(actual)))
    end
end

local n = 0
local function fresh(opts)
    n = n + 1
    return queue.declare("w/" .. n, opts)
end

print("=== queue.wait ===")

print("-- we are running where a program can park")
ok(coroutine.isyieldable(), "the top level is yieldable under --task")

print("-- a wait that can be satisfied now does not park")
do
    local q = fresh{}
    queue.push(q, "ready")
    local id, v, st = queue.wait({q})
    eq(id, q, "the handle that fired is returned")
    eq(v, "ready", "with the message")
    eq(st, "ok", "and status ok")
    eq(queue.len(q), 0, "and the message was taken, not left behind")
end

print("-- wait picks whichever queue has something")
do
    local a, b, c = fresh{}, fresh{}, fresh{}
    queue.push(b, "from b")
    local id, v = queue.wait({a, b, c})
    eq(id, b, "the middle queue fired")
    eq(v, "from b", "with its message")
    -- Readiness beats closure across the whole set: a message already waiting
    -- is more use to the program than a sibling that has gone away.
    queue.push(c, "from c")
    queue.disable(a)
    local id2, v2, st2 = queue.wait({a, c})
    eq(id2, c, "a ready queue wins over a closed sibling")
    eq(st2, "ok", "reporting ok, not closed")
    eq(v2, "from c", "and delivering")
end

print("-- a zero timeout is a poll and never parks")
do
    local q = fresh{}
    local id, v, st = queue.wait({q}, 0)
    eq(id, nil, "nothing fired")
    eq(v, nil, "no message")
    eq(st, "timeout", "reported as a timeout")
end

print("-- a finite timeout elapses, and time really passes")
do
    local q = fresh{}
    local t0 = os.time()
    local id, v, st = queue.wait({q}, 1100)
    eq(st, "timeout", "the wait timed out")
    eq(id, nil, "with no handle")
    -- The host owns the clock (8.3), so this is really asserting that the CLI
    -- reference host honours the duration rather than answering instantly.
    ok(os.time() - t0 >= 1, "and at least a second of wall clock passed")
end

print("-- a queue that can never deliver fires 'closed' (6.4)")
do
    local q = fresh{}
    queue.disable(q)
    local id, v, st = queue.wait({q}, 100)
    eq(id, q, "the closed handle is named")
    eq(v, nil, "with no message")
    eq(st, "closed", "and status closed")

    -- Disabling stops accepting, not delivering: a disabled queue holding a
    -- message still fires ready, and only reports closed once drained.
    local d = fresh{}
    queue.push(d, "last")
    queue.disable(d)
    local _, v2, st2 = queue.wait({d}, 100)
    eq(st2, "ok", "a disabled queue still delivers what it holds")
    eq(v2, "last", "the held message")
    eq(select(3, queue.wait({d}, 100)), "closed", "and only then reports closed")

    local gone = queue.declare("w/destroyed")
    queue.destroy(gone)
    eq(select(3, queue.wait({gone}, 100)), "closed",
       "a destroyed queue fires closed rather than raising")
end

print("-- yielding across pcall works, and must keep working")
do
    -- 8.4's correction. If this ever starts failing, someone has re-added the
    -- restriction the first draft wrongly specified.
    local q = fresh{}
    queue.push(q, "through pcall")
    local okk, id, v, st = pcall(function() return queue.wait({q}) end)
    ok(okk, "pcall returned normally")
    eq(v, "through pcall", "the message came back through the protected call")
    eq(st, "ok", "with status ok")

    -- And a wait that genuinely parks, inside pcall, across a timeout.
    local e = fresh{}
    local okk2, _, _, st2 = pcall(function() return queue.wait({e}, 50) end)
    ok(okk2, "a parking wait inside pcall does not raise")
    eq(st2, "timeout", "and reports its timeout")

    -- xpcall too, since it takes the same continuation path.
    queue.push(q, "again")
    local okk3, _, v3 = xpcall(function() return queue.wait({q}) end, tostring)
    ok(okk3, "xpcall works as well")
    eq(v3, "again", "delivering the message")
end

print("-- the genuinely non-yieldable contexts refuse, by name (8.4)")
do
    local q = fresh{}   -- empty, so any wait on it must park
    local function refuses(f, what)
        local okk, err = pcall(f)
        ok(not okk, what .. " refuses to park")
        ok(tostring(err):find("not yieldable", 1, true) ~= nil,
           what .. " says why: " .. tostring(err))
    end
    refuses(function()
        table.sort({3, 1, 2}, function(a, b) queue.wait({q}, 10) return a < b end)
    end, "a table.sort comparator")
    refuses(function()
        ("x"):gsub("x", function() queue.wait({q}, 10) return "y" end)
    end, "a string.gsub replacement")
    -- 8.4 says "a metamethod invoked from C". That is narrower than it sounds,
    -- and measuring it is the only way to get it right: __index, __concat and
    -- the arithmetic metamethods are dispatched by the VM, which uses
    -- continuations, so a wait inside them PARKS QUITE HAPPILY. Only a
    -- metamethod reached through a C call with no continuation cannot --
    -- __tostring via tostring(), which goes through luaL_callmeta.
    refuses(function()
        tostring(setmetatable({}, {__tostring = function()
            queue.wait({q}, 10)
            return "x"
        end}))
    end, "__tostring called from tostring()")

    -- A __gc finaliser runs at a moment nothing controls, so it is checked by
    -- forcing a collection and catching what the finaliser recorded rather than
    -- by wrapping it in pcall.
    do
        local caught
        do
            local _ = setmetatable({}, {__gc = function()
                local okk, err = pcall(function() queue.wait({q}, 10) end)
                caught = (not okk) and tostring(err) or "no error"
            end})
        end
        collectgarbage("collect")
        collectgarbage("collect")
        ok(caught ~= nil, "the finaliser ran")
        ok(caught == nil or caught:find("not yieldable", 1, true) ~= nil,
           "a __gc finaliser refuses to park: " .. tostring(caught))
    end
end

print("-- a VM-dispatched metamethod IS yieldable, so a wait there parks")
do
    -- The complement of the case above, and the reason 8.4's wording needed
    -- sharpening: this is not a non-yieldable context, and asserting so keeps
    -- anyone from "fixing" it into one.
    local e = fresh{}
    local t = setmetatable({}, {__index = function()
        return select(3, queue.wait({e}, 20))
    end})
    eq(t.anything, "timeout", "a wait inside __index parked and timed out")
    local c = setmetatable({}, {__concat = function()
        return select(3, queue.wait({e}, 20))
    end})
    eq(c .. "", "timeout", "and inside __concat")
end

print("-- but a satisfiable wait works even where a yield would be illegal")
do
    -- This is why the ready check happens before the yieldability check rather
    -- than after: a wait that needs no park should not care whether one was
    -- possible. Without it, ordinary code would have to know which contexts it
    -- was called from before daring to read a queue.
    local q = fresh{}
    queue.push(q, "no park needed")
    local seen
    ("x"):gsub("x", function()
        local _, v = queue.wait({q})
        seen = v
        return "y"
    end)
    eq(seen, "no park needed",
       "a ready wait inside a gsub replacement succeeded")

    -- A zero-timeout poll is likewise always safe, since it never parks.
    local e = fresh{}
    local polled
    ("x"):gsub("x", function()
        polled = select(3, queue.wait({e}, 0))
        return "y"
    end)
    eq(polled, "timeout", "and so is a poll")
end

print("-- on_full = 'block'")
do
    -- Accepted now, where M2 refused it at declare time.
    local q = queue.declare("w/block", {capacity = 2, on_full = "block"})
    eq(queue.info(q).on_full, "block", "the policy is recorded")
    local okk, st = queue.push(q, 1)
    eq(st, "ok", "a push with room behaves like any other")
    eq(select(2, queue.push(q, 2)), "ok", "up to capacity")
    eq(queue.len(q), 2, "and the queue filled")
    -- A push that would block is exercised as a deadlock below, in a
    -- subprocess: under this host nothing can drain a local queue while the
    -- pusher is parked, so blocking here would hang the suite.

    -- Draining first means the next push has room and does not park at all.
    queue.pop(q)
    eq(select(2, queue.push(q, 3)), "ok", "after a pop there is room again")
end

print("-- argument checking")
do
    local q = fresh{}
    ok(not pcall(queue.wait), "wait needs a list")
    ok(not pcall(queue.wait, q), "a bare handle is not a list")
    ok(not pcall(queue.wait, {}), "an empty list is refused")
    ok(not pcall(queue.wait, {q, "nope"}), "a non-handle entry is refused")
    ok(not pcall(queue.wait, {q, 99999}), "an unissued handle is refused")
    local many = {}
    for i = 1, 40 do many[i] = q end
    local okk, err = pcall(queue.wait, many)
    ok(not okk, "more handles than the wait-set holds is refused")
    ok(tostring(err):find("routing", 1, true) ~= nil,
       "and says why that is a routing problem: " .. tostring(err))
end

print("-- deadlock is reported, not hung (checked in a subprocess)")
do
    -- Both of these would hang forever under a host with no other writer, so
    -- they are run as child processes with a timeout. Reporting a deadlock at
    -- the moment it happens is much better than a program that never returns,
    -- and it is the CLI host's answer rather than the runtime's: 8.3 puts the
    -- clock, and therefore this judgement, on the host side.
    -- 'arg[-1]' is only the interpreter when no option precedes the script.
    -- Run as '--task test_wait.lua' it is "--task", so walk to the bottom of
    -- the negative indices instead of assuming.
    local function interpreter()
        local i, last = -1, nil
        while arg and arg[i] do last = arg[i]; i = i - 1 end
        return last
    end
    local interp = interpreter()
    if not (interp and io.popen) then
        print("   (skipped: no interpreter path or io.popen)")
    else
        local function run(src)
            local cmd = string.format("%q --task -e %q 2>&1", interp, src)
            local p = assert(io.popen(cmd))
            local out = p:read("a")
            p:close()
            return out
        end
        local out = run('local q = queue.declare("x") queue.wait({q})')
        ok(out:find("deadlock", 1, true) ~= nil,
           "an indefinite wait on an unwritable queue reports a deadlock")
        local out2 = run(
            'local q = queue.declare("y", {capacity = 1, on_full = "block"}) ' ..
            'queue.push(q, 1) queue.push(q, 2)')
        ok(out2:find("deadlock", 1, true) ~= nil,
           "and so does a blocking push to a queue nothing can drain")
        -- The complement: with a timeout, the same wait returns rather than
        -- deadlocking, which is what distinguishes "cannot proceed" from
        -- "nothing yet".
        local out3 = run(
            'local q = queue.declare("z") print(select(3, queue.wait({q}, 20)))')
        ok(out3:find("timeout", 1, true) ~= nil,
           "the same wait with a timeout reports timeout instead")
    end
end

print("-- an ordinary top-level yield has no host to answer it")
do
    local function interpreter()
        local i, last = -1, nil
        while arg and arg[i] do last = arg[i]; i = i - 1 end
        return last
    end
    local interp = interpreter()
    if interp and io.popen then
        local p = assert(io.popen(string.format(
            "%q --task -e %q 2>&1", interp, "coroutine.yield()")))
        local out = p:read("a")
        p:close()
        ok(out:find("wait%-set") ~= nil or out:find("yield", 1, true) ~= nil,
           "and says so rather than treating it as a wait: " .. out:gsub("\n", " "))
    end
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
