-- test_interop.lua
-- Diluvium's features are developed one at a time; this is where they meet.
-- Two things are checked here that no single-feature test can check:
-- combinations (does an f-string inside a switch inside a defer work?), and
-- backward compatibility (does every contextual keyword still work as an
-- ordinary name, in every position stock Lua allows?).

local function assert_eq(a, e, n)
    if a == e then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s\n  expected '%s'\n  actual   '%s'",
         n, tostring(e), tostring(a))); os.exit(1) end
end
local function assert_compiles(src, n)
    local f, err = load(src)
    if f then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s\n  %s", n, tostring(err))); os.exit(1) end
end
local function assert_nocompile(src, n)
    if load(src) == nil then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s (compiled)", n)); os.exit(1) end
end

print("=== Starting Interop Tests ===\n")

-- 1. Backward compatibility ------------------------------------------------
-- Every contextual keyword has to survive being an ordinary name. These are
-- the shapes real Lua code uses them in.

print("-- 1. Contextual keywords as ordinary names")
for _, kw in ipairs{"switch", "defer", "with", "case", "default"} do
    assert_compiles(("local %s = 1 return %s + 1"):format(kw, kw),
                    kw .. " as a local")
    assert_compiles(("local t = {} t.%s = 1 return t.%s"):format(kw, kw),
                    kw .. " as a field")
    assert_compiles(("local t = {%s = 1} return t"):format(kw),
                    kw .. " as a constructor key")
    assert_compiles(("local function %s() end %s()"):format(kw, kw),
                    kw .. " as a function name")
    assert_compiles(("local t = {} function t:%s() return self end"):format(kw),
                    kw .. " as a method name")
    assert_compiles(("local %s = function() end return %s(1, 2)"):format(kw, kw),
                    kw .. " called with arguments")
end

-- The three call shapes the switch lookahead deliberately excludes: these
-- have to stay function calls, not statements introducing a subject.
local calls = 0
local function switch(x) calls = calls + 1; return x end
local defer, with = switch, switch
switch (1)
switch "s"
switch {}
defer (1)
with (1)
assert_eq(calls, 5, "call shapes still call")

-- 2. Combinations ----------------------------------------------------------

print("\n-- 2. Features in combination")

-- f-string with a format spec, built from a safe-navigation chain and a
-- null-coalescing default, inside a switch case, inside a deferred block.
local log = {}
local function run(cfg, code)
    local out
    do
        defer do
            local port = cfg?.server?.port ?? 8080
            switch code do
                case 200, 204 then
                    out = $"ok {port::%05d}"
                case 404 then
                    out = $"missing {cfg?.name ?? "anon"}"
                default
                    out = $"? {code}"
            end
            log[#log+1] = out
        end
    end
    return out
end

assert_eq(run({server = {port = 443}}, 200), "ok 00443", "spec + ?. + ?? in a case in a defer")
assert_eq(run(nil, 200), "ok 08080", "?? supplies the default through a nil chain")
assert_eq(run({name = "svc"}, 404), "missing svc", "nested f-string quotes in a case")
assert_eq(run(nil, 404), "missing anon", "?? inside an f-string interpolation")
assert_eq(run(nil, 500), "? 500", "default arm")
assert_eq(#log, 5, "the deferred block ran once per call")

-- Compound assignment against a safe-navigation read, and against a field
-- whose prefix is a call (evaluated once).
local prefix_calls = 0
local counters = {n = 0}
local function target() prefix_calls = prefix_calls + 1; return counters end
target().n += 5
target().n *= 3
assert_eq(counters.n, 15, "compound assignment on a called prefix")
assert_eq(prefix_calls, 2, "the prefix is evaluated once per statement")

local settings = nil
local timeout = settings?.timeout ?? 0
timeout += 10
assert_eq(timeout, 10, "?? result is an ordinary local afterwards")

-- with, holding a resource used by an f-string and closed on an error path.
local closed = {}
local function res(name)
    return setmetatable({name = name},
        {__close = function(s) closed[#closed+1] = s.name end})
end

local ok, err = pcall(function()
    with a = res("a"), b = res("b") do
        error($"failed on {b.name}")
    end
end)
assert_eq(ok, false, "the error propagates out of with")
assert_eq(err:match("failed on b$") ~= nil, true, "f-string built the message")
assert_eq(table.concat(closed, ","), "b,a", "both closed, in reverse, while unwinding")

-- switch on a value produced by a secure function, with defers in the arms.
~function classify(n)
    switch n % 3 do
        case 0 then return "zero"
        case 1 then return "one"
        default return "two"
    end
end
assert_eq(classify(9) .. classify(10) .. classify(11), "zeroonetwo",
          "switch inside a secure function")

-- Deferred statements ordering across a loop with break, mixed with with.
log = {}
for i = 1, 3 do
    defer log[#log+1] = "d" .. i
    with r = res("w" .. i) do
        log[#log+1] = "b" .. i
        if i == 2 then break end
    end
end
assert_eq(table.concat(log, ","), "b1,d1,b2,d2", "defer and with both run on break")

-- 3. Compatibility guarantees ----------------------------------------------

print("\n-- 3. New syntax stays a syntax error in stock Lua")
-- Not testable from inside Diluvium, but the inverse is: constructs that must
-- NOT compile, because accepting them would make a stock-Lua program mean
-- something different here.
assert_nocompile("local x = 1 x ~= 2", "no compound xor form")
assert_nocompile("local t = {} t?.x = 1", "a safe-navigation chain is not assignable")
assert_nocompile("switch 1 do case 1 then", "an unterminated switch is rejected")
assert_nocompile("with x = 1 print(x) end", "with requires do")

print("\n=== All Interop Tests Passed ===")
