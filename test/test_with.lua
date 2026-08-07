-- test_with.lua
-- 'with' binds to-be-closed locals scoped to a block.

local function assert_eq(a, e, n)
    if a == e then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s\n  expected '%s'\n  actual   '%s'",
         n, tostring(e), tostring(a))); os.exit(1) end
end
local function assert_nocompile(src, n)
    if load(src) == nil then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s (compiled)", n)); os.exit(1) end
end

print("=== Starting With Tests ===\n")
local log
local function say(s) log[#log+1] = s end
local function res(name) return setmetatable({}, {__close = function() say(name) end}) end

print("-- 1. Closes at the end of the block")
log = {}
with a = res("a") do say("body") end
assert_eq(table.concat(log, ","), "body,a", "closed after the block")

log = {}
with a = res("a"), b = res("b") do say("body") end
assert_eq(table.concat(log, ","), "body,b,a", "several bindings close in reverse")

print("-- 2. Every way out")
log = {}
local ok = pcall(function() with a = res("a") do error("x") end end)
assert_eq(ok, false, "an error propagates")
assert_eq(table.concat(log, ","), "a", "...and it still closed")

log = {}
local function f() with a = res("a") do return "v" end end
assert_eq(f(), "v", "return value untouched")
assert_eq(table.concat(log, ","), "a", "closed on return")

log = {}
for i = 1, 3 do with a = res("i" .. i) do if i == 2 then break end end end
assert_eq(table.concat(log, ","), "i1,i2", "closed each iteration, and on break")

log = {}
do with a = res("g") do goto out end end
::out::
assert_eq(table.concat(log, ","), "g", "closed on goto")

print("-- 3. Scoping")
local outer = "outer"
with outer = res("shadow") do
    assert_eq(type(outer), "table", "the binding shadows inside the block")
end
assert_eq(outer, "outer", "...and not outside it")

log = {}
with a = res("a") do
    with b = res("b") do say("inner") end
    say("between")
end
assert_eq(table.concat(log, ","), "inner,b,between,a", "nested blocks")

print("-- 4. Alongside defer and <close>")
log = {}
with a = res("a") do
    defer say("defer")
    local u <close> = res("user")
    say("body")
end
assert_eq(table.concat(log, ","), "body,user,defer,a",
          "with, defer and <close> share one reverse order")

print("-- 5. A non-closable value raises, as <close> does")
assert_eq(pcall(load("with x = 5 do end")), false, "a plain number is rejected")

print("-- 6. 'with' is still an ordinary name")
local with = function(a) return "call:" .. type(a) end
assert_eq(with(1), "call:number", "call with parentheses")
assert_eq(with "s", "call:string", "call with a string")
with = 5
assert_eq(with, 5, "assignment")
local t = {with = 7}
assert_eq(t.with, 7, "field named 'with'")
local function takes(with) return with * 2 end
assert_eq(takes(21), 42, "parameter named 'with'")
do
    local seen
    local with = function(a) seen = a end
    with (42)
    assert_eq(seen, 42, "'with (x)' is still a call")
end

print("-- 7. Malformed")
assert_nocompile([[with a = 1 print(a)]], "missing 'do'")
assert_nocompile([[with a do end]], "missing '='")
assert_nocompile([[with a = 1 do]], "unclosed block")

print("\n=== All Tests Passed! ===")
