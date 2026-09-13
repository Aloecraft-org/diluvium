-- test_params.lua
-- Verifies default parameter values (syntax proposals 3.2) and
-- expression-bodied functions (3.8). Together because they are the two
-- forms that change what a function header may look like, and each has to
-- keep working when the other is present.

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

print("=== Starting Parameter and Expression-Body Tests ===\n")

print("-- 1. Default values")
local function connect(host, port = 8080, tls = true)
    return string.format("%s:%d:%s", host, port, tostring(tls))
end
assert_eq(connect("h"), "h:8080:true", "both defaults")
assert_eq(connect("h", 99), "h:99:true", "one supplied")
assert_eq(connect("h", 99, false), "h:99:false", "all supplied")
-- A missing argument and an explicit nil are the same value in Lua, so
-- they take the same path here.
assert_eq(connect("h", nil, nil), "h:8080:true", "explicit nil takes the default")
assert_eq(connect("h", nil, false), "h:8080:false",
          "a default fills in around a supplied 'false'")
local function flag(on = true) return on end
assert_eq(flag(false), false, "'false' is a value, not a missing argument")

print("-- 2. A default may name the parameters to its left")
local function rect(w, h = w) return w * h end
assert_eq(rect(4), 16, "the earlier parameter is in scope")
assert_eq(rect(4, 5), 20, "and is not used when an argument is given")

print("-- 3. Defaults are evaluated per call, and only when needed")
local calls = 0
local function counted(x = (function() calls = calls + 1; return calls end)())
    return x
end
assert_eq(counted(), 1, "first call evaluates it")
assert_eq(counted(), 2, "second call evaluates it again")
assert_eq(counted("given"), "given", "a supplied argument skips it")
assert_eq(calls, 2, "so it ran exactly twice")

local function need(x = error("x is required", 0)) return x end
assert_eq(select(2, pcall(need)), "x is required", "a default may raise")
assert_eq(select(2, pcall(need, 1)), 1, "and does not when an argument is given")

print("-- 4. Methods")
local o = {factor = 10}
function o:scaled(k = 2, b = k) return self.factor * k + b end
assert_eq(o:scaled(), 22, "'self' and both defaults")
assert_eq(o:scaled(3), 33, "one supplied, the second default sees it")
assert_eq(o:scaled(3, 1), 31, "all supplied")

print("-- 5. '...' cannot follow a default")
-- OP_VARARGPREP reads the live argument count off the stack, so it has to
-- be the function's first instruction, and a default's code is already in
-- front of it by the time '...' is read.
assert_nocompile("local function f(a = 1, ...) end", "default then '...'")
local function stillvararg(a, ...) return a, select("#", ...) end
assert_eq(select(2, stillvararg(1, 2, 3)), 2, "plain varargs are untouched")
local function namedvararg(...xs) return #xs end
assert_eq(namedvararg(9, 8), 2, "and so is a named vararg table")

print("-- 6. Expression bodies")
local function area(w, h) = w * h
assert_eq(area(3, 4), 12, "local function")

function GlobalArea(w, h) = w * h
assert_eq(GlobalArea(3, 4), 12, "global function")

local Point = {}
Point.__index = Point
function Point:len() = math.sqrt(self.x ^ 2 + self.y ^ 2)
assert_eq(setmetatable({x = 3, y = 4}, Point):len(), 5.0, "method")

assert_eq((function(x) = x + 1)(41), 42, "anonymous, called at once")

local xs = {3, 1, 2}
table.sort(xs, function(a, b) = a < b)
assert_eq(table.concat(xs, ","), "1,2,3", "as an argument")

print("-- 7. An expression body ends where its expression ends")
-- The reason it is one expression and not a list: there is no 'end' to
-- close it, so the comma has to belong to the table.
local t = {half = function(x) = x / 2, name = "half"}
assert_eq(t.name, "half", "the comma belongs to the constructor")
assert_eq(t.half(10), 5.0, "and the function is still the function")

print("-- 8. The two forms together")
local function scale(x, k = 3) = x * k
assert_eq(scale(2), 6, "a default and an expression body")
assert_eq(scale(2, 10), 20, "with the argument supplied")

print("-- 9. A call in an expression body is still a tail call")
-- 'istailcall' is the interpreter's own answer, so this asserts the
-- optimisation rather than the syntax that asked for it.
local function inner() = debug.getinfo(1, "t").istailcall
local function outer() = inner()
assert_eq(outer(), true, "the call in the body is a tail call")
local function notail() local r = inner() return r end
assert_eq(notail(), false,
          "and the same call, not in tail position, is not one")
local function three() return 1, 2, 3 end
local function all() = three()
assert_eq(select("#", all()), 3, "every value passes through")
assert_eq(select(3, all()), 3, "and they are the right ones")

print("\n=== All Parameter and Expression-Body Tests Passed ===")
