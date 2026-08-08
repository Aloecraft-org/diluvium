-- Diluvium benchmark harness.
--
-- Run:  diluvium script/bench.lua [--json] [--scale N]
--
-- Reports two numbers per case. The one that matters is the VM instruction
-- count: it is deterministic, so the same code on the same build gives the
-- same figure on any machine, and a change in it is a real change in work
-- done. Wall-clock is recorded beside it but is advisory -- shared CI
-- runners vary by more than most regressions worth catching, so timing
-- alone cannot tell a slower build from a busier machine.
--
-- Counting is done with a count hook rather than a patched VM, so this is
-- ordinary Lua and runs anywhere Diluvium runs, WASI included. The hook
-- fires every HOOK_STEP instructions; totals are therefore exact to within
-- HOOK_STEP, which against millions of instructions is noise. The timed
-- pass runs with no hook installed, so timing is not distorted by counting.
--
-- Cases deliberately include Diluvium's own constructs. Tracking only
-- stock Lua would leave the cost of the features this fork exists for
-- unmeasured.

local HOOK_STEP = 1000

local args = {...}
local as_json, scale = false, 1
for i = 1, #args do
    if args[i] == "--json" then as_json = true
    elseif args[i] == "--scale" then scale = tonumber(args[i + 1]) or 1 end
end

local function n(base) return math.max(1, math.floor(base * scale)) end

-- Cases -------------------------------------------------------------------
-- Each returns a value, so nothing can be optimised away as dead, and takes
-- its own iteration count so one slow case can be scaled independently.

local cases = {}
local function case(name, iters, fn) cases[#cases + 1] = {name = name, iters = iters, fn = fn} end

case("arith", 400000, function (N)
    local sum = 0
    for i = 0, N - 1 do
        if i % 2 == 0 then sum = sum + (i / 2) else sum = sum + (i * 3) + 1 end
    end
    return sum
end)

case("call", 200000, function (N)
    local function add3(a, b, c) return a + b + c end
    local acc = 0
    for i = 1, N do acc = acc + add3(i, i + 1, i + 2) end
    return acc
end)

case("recurse", 22, function (N)
    local function fib(k) if k <= 1 then return k end return fib(k - 1) + fib(k - 2) end
    return fib(N)
end)

case("table", 200000, function (N)
    local t, acc = {}, 0
    for i = 1, N do t[i] = i * 2 end
    for i = 1, N do acc = acc + t[i] end
    t = {}
    for i = 1, N // 10 do t["k" .. i] = i end
    for i = 1, N // 10 do acc = acc + t["k" .. i] end
    return acc
end)

case("string", 60000, function (N)
    local s, acc = "", 0
    for i = 0, N - 1 do
        if #s > 100 then acc = acc + #s; s = "" end
        s = s .. tostring(i)
    end
    return acc + #s
end)

case("pattern", 20000, function (N)
    local subject, acc = "the quick brown fox jumps over the lazy dog", 0
    for _ = 1, N do
        acc = acc + #subject:gsub("(%w+)%s(%w+)", "%2 %1")
        acc = acc + (subject:find("jumps") or 0)
    end
    return acc
end)

case("sort", 300, function (N)
    local acc = 0
    for _ = 1, N do
        local t = {}
        for i = 1, 500 do t[i] = (i * 7919) % 1000 end
        table.sort(t)
        acc = acc + t[1] + t[500]
    end
    return acc
end)

-- Diluvium's own constructs, so their cost is tracked rather than assumed.

case("fstring", 100000, function (N)
    local acc = 0
    for i = 1, N do
        local s = $"item {i} of {N}"
        acc = acc + #s
    end
    return acc
end)

case("fstring_spec", 100000, function (N)
    local acc = 0
    for i = 1, N do
        local s = $"value {i / 7::%.3f}"
        acc = acc + #s
    end
    return acc
end)

case("switch", 300000, function (N)
    local acc = 0
    for i = 1, N do
        switch i % 5 do
            case 0 then acc = acc + 1
            case 1, 2 then acc = acc + 2
            case 3 then acc = acc + 3
            default acc = acc + 4
        end
    end
    return acc
end)

case("safenav", 300000, function (N)
    local cfg = {server = {port = 8080}}
    local acc = 0
    for i = 1, N do
        acc = acc + (cfg?.server?.port ?? 0) + (cfg?.missing?.port ?? 1)
        if i % 2 == 0 then acc = acc + ((nil)?.a?.b ?? 1) end
    end
    return acc
end)

case("defer", 100000, function (N)
    local acc = 0
    for _ = 1, N do
        do
            defer acc = acc + 1
            acc = acc + 1
        end
    end
    return acc
end)

case("dump_load", 400, function (N)
    local src = [[
        local t = {}
        local function helper(a, b) return a * b + #t end
        ~function secret() return "hidden-" .. helper(2, 3) end
        return secret, helper
    ]]
    local chunk = assert(load(src))
    local bytes = string.dump(chunk)
    local acc = 0
    for _ = 1, N do
        acc = acc + #string.dump(assert(load(src)))
        local secret = assert(load(bytes))()
        acc = acc + #secret()
    end
    return acc
end)

-- Measurement --------------------------------------------------------------

local function count_instructions(fn, iters)
    local hits = 0
    debug.sethook(function () hits = hits + 1 end, "", HOOK_STEP)
    local ok, err = pcall(fn, iters)
    debug.sethook()
    if not ok then error(err, 0) end
    return hits * HOOK_STEP
end

local function time_ms(fn, iters)
    collectgarbage("collect")
    local before = collectgarbage("count")
    local t0 = os.clock()
    local ok, err = pcall(fn, iters)
    local elapsed = os.clock() - t0
    if not ok then error(err, 0) end
    return elapsed * 1000, collectgarbage("count") - before
end

local results = {}
for _, c in ipairs(cases) do
    local iters = n(c.iters)
    -- Timed first, with no hook installed; counted after.
    local ms, gc_kb = time_ms(c.fn, iters)
    local instr = count_instructions(c.fn, iters)
    results[#results + 1] = {
        name = c.name, iters = iters, instructions = instr,
        ms = ms, gc_kb = gc_kb,
    }
end

-- Output --------------------------------------------------------------------

if as_json then
    local parts = {}
    for _, r in ipairs(results) do
        parts[#parts + 1] = string.format(
            '    "%s": { "iters": %d, "instructions": %d, "ms": %.2f, "gc_kb": %.1f }',
            r.name, r.iters, r.instructions, r.ms, r.gc_kb)
    end
    io.write("{\n")
    io.write(string.format('  "hook_step": %d,\n', HOOK_STEP))
    io.write(string.format('  "scale": %s,\n', tostring(scale)))
    io.write(string.format('  "version": "%s",\n', _VERSION))
    io.write('  "cases": {\n', table.concat(parts, ",\n"), "\n  }\n}\n")
else
    print($"Diluvium benchmarks  (scale {scale}, hook step {HOOK_STEP})")
    print("")
    print(string.format("%-14s %14s %10s %10s", "case", "instructions", "ms", "gc KB"))
    print(string.rep("-", 52))
    local total = 0
    for _, r in ipairs(results) do
        total = total + r.instructions
        print(string.format("%-14s %14d %10.1f %10.1f",
              r.name, r.instructions, r.ms, r.gc_kb))
    end
    print(string.rep("-", 52))
    print(string.format("%-14s %14d", "total", total))
    print("")
    print("Instruction counts are deterministic and are the comparable")
    print("number. Times are advisory: a shared runner varies by more.")
end
