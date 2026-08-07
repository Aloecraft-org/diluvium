-- test_compound.lua
-- Verifies compound assignment: every operator, single evaluation of the
-- target's prefix, and that nothing stock Lua accepts changed meaning.

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

print("=== Starting Compound Assignment Tests ===\n")

print("-- 1. Arithmetic operators")
local x = 10
x += 5   assert_eq(x, 15, "+=")
x -= 3   assert_eq(x, 12, "-=")
x *= 2   assert_eq(x, 24, "*=")
x /= 4   assert_eq(x, 6.0, "/= (always a float, as '/' is)")
x = 17
x //= 5  assert_eq(x, 3, "//=")
x %= 2   assert_eq(x, 1, "%=")
x = 2
x ^= 3   assert_eq(x, 8.0, "^= (always a float, as '^' is)")

print("-- 2. Bitwise operators")
local b = 0xF0
b |= 0x0F   assert_eq(b, 0xFF, "|=")
b &= 0x3C   assert_eq(b, 0x3C, "&=")
b <<= 2     assert_eq(b, 0xF0, "<<=")
b >>= 1     assert_eq(b, 0x78, ">>=")

print("-- 3. Concatenation")
local s = "a"
s ..= "b"
s ..= "c" .. "d"
assert_eq(s, "abcd", "..=")

print("-- 4. Null coalescing")
local n = nil
n ??= "first"   assert_eq(n, "first", "??= assigns when nil")
n ??= "second"  assert_eq(n, "first", "??= leaves a non-nil value alone")
local f = false
f ??= "not applied"
assert_eq(f, false, "??= treats false as a value, not as nil")

do  -- ??= must not evaluate its right-hand side when the target is set
    local ran = false
    local function rhs() ran = true return 1 end
    local set = "already"
    set ??= rhs()
    assert_eq(ran, false, "??= short-circuits its right-hand side")
    assert_eq(set, "already", "...and keeps the original value")
end

print("-- 5. The target's prefix is evaluated exactly once")
local calls = 0
local function key() calls = calls + 1 return "k" end
local t = {k = 1}
t[key()] += 10
assert_eq(t.k, 11, "indexed compound assignment applies")
assert_eq(calls, 1, "the key expression ran once, not twice")

calls = 0
local function tbl() calls = calls + 1 return t end
tbl().k *= 2
assert_eq(t.k, 22, "compound assignment through a call result")
assert_eq(calls, 1, "the table expression ran once")

print("-- 6. Every kind of assignable target")
do
    local loc = 1
    loc += 1
    assert_eq(loc, 2, "local variable")
end
gvar = 1
gvar += 41
assert_eq(gvar, 42, "global variable")
gvar = nil
do
    local up = 1
    local function bump() up += 9 end
    bump()
    assert_eq(up, 10, "upvalue")
end
do
    local rec = {n = 1, sub = {n = 2}}
    rec.n += 1
    assert_eq(rec.n, 2, "field access")
    rec.sub.n += 1
    assert_eq(rec.sub.n, 3, "nested field access")
    rec["n"] += 1
    assert_eq(rec.n, 3, "string-key index")
    local arr = {1, 2, 3}
    arr[2] += 10
    assert_eq(arr[2], 12, "integer index")
end

print("-- 7. Metamethods behave as they do in the longhand form")
do
    local mt = {__add = function(a, b) return setmetatable({v = a.v + b}, getmetatable(a)) end}
    local o = setmetatable({v = 1}, mt)
    o += 4
    assert_eq(o.v, 5, "__add is used")
end
do
    local log = {}
    local proxy = setmetatable({}, {
        __index = function(_, k) log[#log + 1] = "get" return 5 end,
        __newindex = function(_, k, v) log[#log + 1] = "set:" .. v end,
    })
    proxy.field += 1
    assert_eq(table.concat(log, ","), "get,set:6", "__index then __newindex, once each")
end

print("-- 8. The right-hand side is a full expression")
local r = 1
r += 2 * 3
assert_eq(r, 7, "right-hand side keeps normal precedence")
r = 2
r *= 1 + 1
assert_eq(r, 4, "...so 'r *= 1 + 1' multiplies by 2, not by 1")
local base = 10
r = 1
r += base > 5 and 100 or 1
assert_eq(r, 101, "and/or on the right-hand side")

print("-- 9. Constants cannot be compound-assigned")
assert_nocompile([[local c <const> = 1  c += 1]], "<const> local rejected")
assert_nocompile([[local c <close> = nil  c += 1]], "<close> local rejected")

print("-- 10. Not an expression, and not valid on a non-target")
assert_nocompile([[local a = (1 += 2)]], "not an expression")
assert_nocompile([[f() += 1]], "a call is not assignable")
assert_nocompile([["s" += 1]], "a literal is not assignable")
assert_nocompile([[local a = 1  a +=]], "missing right-hand side")

print("-- 11. Nothing stock Lua accepts changed meaning")
-- '~=' stays 'not equal'; there is deliberately no xor-assign.
local ne = (1 ~= 2)
assert_eq(ne, true, "'~=' is still not-equal")
do
    local v = 5
    -- A bare comparison is still a comparison, not an assignment.
    assert_eq(v == 5, true, "'==' unaffected")
    assert_eq(v >= 5, true, "'>=' unaffected")
    assert_eq(v <= 5, true, "'<=' unaffected")
    assert_eq(v // 2, 2, "'//' unaffected")
    assert_eq(v << 1, 10, "'<<' unaffected")
    assert_eq(v >> 1, 2, "'>>' unaffected")
    assert_eq("a" .. "b", "ab", "'..' unaffected")
    assert_eq(nil ?? "d", "d", "'??' unaffected")
end
do  -- ordinary assignment of a negative/parenthesised value still parses
    local a = 1
    a = -1        assert_eq(a, -1, "assignment of a negation")
    a = (2)       assert_eq(a, 2, "assignment of a parenthesised value")
    local p, q = 1, 2
    p, q = q, p
    assert_eq(p .. "," .. q, "2,1", "multiple assignment unaffected")
end

print("\n=== All Tests Passed! ===")
