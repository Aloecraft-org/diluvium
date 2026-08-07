-- test_safenav.lua
-- Verifies safe navigation: '?.' and '?[' short-circuit the whole
-- remaining chain on nil, test nil specifically rather than falsiness,
-- and leave every stock-Lua use of '?' and '??' alone.

local function assert_eq(actual, expected, name)
    if actual == expected then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected: '%s'", tostring(expected)))
        print(string.format("       Actual:   '%s'", tostring(actual)))
        os.exit(1)
    end
end

local function assert_nocompile(src, name)
    if load(src) == nil then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        os.exit(1)
    end
end

print("=== Starting Safe Navigation Tests ===\n")

local t = {
    a = {b = {c = "deep"}},
    n = 5,
    s = "str",
    f = function(x) return "f:" .. tostring(x) end,
    m = {greet = function(self, who) return "hi " .. who end},
}
local nope = nil

print("-- 1. Present values behave like ordinary indexing")
assert_eq(t?.n, 5, "single '?.' on a present field")
assert_eq(t?.a?.b?.c, "deep", "a chain of '?.'")
assert_eq(t?.a.b.c, "deep", "'?.' then ordinary '.'")
assert_eq(t?["n"], 5, "'?[' on a present key")
assert_eq(t?.a["b"]?.c, "deep", "'?.' and '?[' and '.' mixed")

print("-- 2. A nil head yields nil")
assert_eq(nope?.a, nil, "'?.' on nil")
assert_eq(nope?["a"], nil, "'?[' on nil")
assert_eq(t?.missing?.deeper, nil, "'?.' on a missing field")

print("-- 3. The WHOLE remaining chain is skipped, not just one step")
-- Each of these would raise "attempt to index a nil value" if only the
-- step immediately after the '?' were guarded.
assert_eq(nope?.a.b.c, nil, "plain '.' after a nil '?.' does not index")
assert_eq(nope?.a["b"].c, nil, "'[' after a nil '?.' does not index")
assert_eq(t.missing?.a.b.c, nil, "the head can be a missing field")
assert_eq(nope?.a.b.c.d.e.f, nil, "however long the chain is")

print("-- 4. Calls in a skipped chain do not happen")
local called = false
local rec = {go = function() called = true return "ran" end}
assert_eq(rec?.go(), "ran", "the call happens when the head is present")
assert_eq(called, true, "...and its side effect is visible")
called = false
assert_eq(nope?.go(), nil, "the call is skipped when the head is nil")
assert_eq(called, false, "...and its side effect never happens")

local argused = false
local function arg() argused = true return 1 end
assert_eq(nope?.go(arg()), nil, "a skipped call yields nil")
assert_eq(argused, false, "...and its arguments are never evaluated")

assert_eq(t?.m:greet("you"), "hi you", "a method call after '?.'")
assert_eq(t?.f("x"), "f:x", "a plain call after '?.'")

print("-- 5. nil, not falsiness")
-- 'false' is a value; indexing it is an error, and '?.' must not hide that.
local isfalse = false
local ok, err = pcall(function() return isfalse?.x end)
assert_eq(ok, false, "'?.' on false still raises")
assert_eq(err:match("attempt to index") ~= nil, true, "...as an index error")
assert_eq(pcall(function() local n = 0 return n?.x end), false,
          "'?.' on a number raises rather than yielding nil")

print("-- 6. Statement position")
local ran = false
local h = {go = function() ran = true end}
h?.go()
assert_eq(ran, true, "'a?.b()' works as a statement")
ran = false
local nilh = nil
nilh?.go()
assert_eq(ran, false, "'a?.b()' on nil is a statement that does nothing")

print("-- 7. Composes with the rest of the language")
assert_eq(nope?.a ?? "fallback", "fallback", "'?.' feeding '??'")
assert_eq(t?.a?.zzz ?? "defaulted", "defaulted", "a missing tail feeding '??'")
assert_eq(t?.n + 1, 6, "the result is an ordinary value")
assert_eq($"v={nope?.a}", "v=nil", "inside an f-string")
assert_eq(tostring(nope?.a), "nil", "as a call argument")
do
    local acc = 0
    for _ = 1, 2 do acc = acc + (t?.n or 0) end
    assert_eq(acc, 10, "inside a loop")
end
do
    local hit = "none"
    switch nope?.a do
        case nil then hit = "nil-case"
    end
    assert_eq(hit, "nil-case", "as a switch subject")
end

print("-- 8. Not an assignment target")
assert_nocompile([[local t = {} t?.b = 1]], "'a?.b = 1' rejected")
assert_nocompile([[local t = {} t?["b"] = 1]], "'a?[k] = 1' rejected")
assert_nocompile([[local t = {} t?.a, t.b = 1, 2]], "rejected in a multiple assignment")

print("-- 9. Stock Lua's use of '?' is unaffected")
assert_nocompile([[local a = 1 ? 2]], "a bare '?' is still a syntax error")
assert_nocompile([[local t = {} return t?]], "'?' with nothing to index is still an error")
assert_eq(nil ?? "ok", "ok", "'??' still means null coalescing")
assert_eq(false ?? "ok", false, "'??' still treats false as a value")
do
    local v = nil
    v ??= "set"
    assert_eq(v, "set", "'??=' still works")
end

print("-- 10. Evaluation order and single evaluation of the head")
do
    local calls = 0
    local function head() calls = calls + 1 return {x = {y = "z"}} end
    assert_eq(head()?.x.y, "z", "a call as the head of the chain")
    assert_eq(calls, 1, "the head is evaluated once")
    calls = 0
    local function nilhead() calls = calls + 1 return nil end
    assert_eq(nilhead()?.x.y, nil, "a nil-returning call as the head")
    assert_eq(calls, 1, "...still evaluated exactly once")
end

print("\n=== All Tests Passed! ===")
