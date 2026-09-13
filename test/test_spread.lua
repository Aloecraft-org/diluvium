-- test_spread.lua
-- Verifies spread (syntax proposals 3.7): where it is allowed, that it
-- expands to every value, that a spread which is not last is refused
-- rather than silently truncated, and that '...' keeps every meaning it
-- already had.

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
    local fn, err = load(src)
    if fn == nil then
        print(string.format("[PASS] %s (%s)", name, err))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        os.exit(1)
    end
end

print("=== Starting Spread Tests ===\n")

local args = {2, 3, 4}
local none = {}
local function four(a, b, c, d)
    return string.format("%s|%s|%s|%s", tostring(a), tostring(b),
                         tostring(c), tostring(d))
end

print("-- 1. In a call")
assert_eq(four(...args), "2|3|4|nil", "the whole argument list")
assert_eq(four(1, ...args), "1|2|3|4", "after other arguments")
assert_eq(select("#", ...args), 3, "expands to every value")
assert_eq(select("#", ...none), 0, "an empty table expands to none")

print("-- 2. In a constructor")
local rest = {"b", "c"}
local t = {"a", ...rest}
assert_eq(table.concat(t, ","), "a,b,c", "after other items")
assert_eq(#t, 3, "with the right length")
assert_eq(#{...rest}, 2, "on its own")
assert_eq(#{...none}, 0, "and empty stays empty")

print("-- 3. What may be spread")
-- A name and its suffixes, which is what the form is written as. '...{}'
-- and '...(f())' are not spreads; they are refused, and the rule stays
-- one sentence.
assert_nocompile("print(...{})", "a constructor is not a spread target")
local box = {inner = {7, 8}}
assert_eq(four(...box.inner), "7|8|nil|nil", "a field")
local boxes = {{5, 6}}
assert_eq(four(...boxes[1]), "5|6|nil|nil", "an index")
local function get() return {1, 2} end
assert_eq(four(...get()), "1|2|nil|nil", "a call's result")
-- 'suffixedexp', not 'expr': the spread takes a variable and its
-- suffixes and stops, so a binary operator after it is not part of it.
assert_eq(#{...rest} + 1, 3, "a following operator is not part of the spread")

print("-- 4. A spread must be last")
-- Lua expands only the last expression of a list; an earlier one is
-- truncated to one value. Refused rather than silently dropping the rest.
assert_nocompile("local a = {} return {...a, 1}",
                 "not last in a constructor")
assert_nocompile("local a, b = {}, {} return {...a, ...b}",
                 "two spreads in a constructor")
assert_nocompile("local a = {} print(...a, 1)", "not last in a call")

print("-- 5. Where a spread is not offered, it is refused")
-- A list with a fixed arity would truncate it without saying so.
assert_nocompile("local a = {} local x = ...a", "a local declaration")
assert_nocompile("local a = {} return ...a", "a return statement")
assert_nocompile("local a = {} local x x = ...a", "an assignment")

print("-- 6. '...' keeps every meaning it had")
local function counted(...) return select("#", ...) end
assert_eq(counted(1, 2, 3), 3, "varargs")
local function packed(...) return #{...} end
assert_eq(packed(1, 2), 2, "varargs in a constructor")
local function named(...xs) return #xs, xs[1] end
assert_eq(select(1, named(9, 8)), 2, "a named vararg parameter")
local function forward(...) return counted(...) end
assert_eq(forward(1, 2, 3, 4), 4, "forwarding varargs")
-- Both in one function: '...' is the varargs, '...name' is a spread.
local function mixed(...)
    local collected = {...}
    return four(1, ...collected)
end
assert_eq(mixed(2, 3), "1|2|3|nil", "varargs and a spread together")

print("\n=== All Spread Tests Passed ===")
