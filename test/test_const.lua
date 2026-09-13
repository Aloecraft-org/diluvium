-- test_const.lua
-- Verifies the const declaration (syntax proposals 3.6): that it is
-- exactly 'local NAME <const>', including the compile-time folding that
-- attribute buys, and that 'const' is still an ordinary name.

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

print("=== Starting Const Tests ===\n")

print("-- 1. Declaring")
const RATE = 0.05
assert_eq(RATE, 0.05, "one name")
const A, B = 1, 2
assert_eq(A + B, 3, "a list")
const T = {1, 2}
assert_eq(T[1], 1, "a table value")
T[1] = 9  -- the binding is constant, the value is not
assert_eq(T[1], 9, "the value it names is still mutable")

print("-- 2. It is the '<const>' attribute")
assert_nocompile("const X = 1; X = 2", "assignment is refused")
assert_nocompile("local Y <const> = 1; Y = 2", "exactly as '<const>' is")
-- An uninitialised '<const>' is legal in Lua and binds nil, so 'const'
-- binds nil too rather than inventing a rule the attribute does not have.
assert_eq(load("const Z return Z")(), load("local Z <const> return Z")(),
          "an uninitialised const binds nil, as '<const>' does")

-- A '<const>' whose value is a compile-time constant is folded rather
-- than stored, and 'const' inherits that because it is the same code
-- path: a fold means no register and no local.
local function nlocals(src)
    local f = assert(load(src))
    local n = 0
    while debug.getlocal(f, n + 1) do n = n + 1 end
    return n
end
assert_eq(nlocals("const K = 10 return K + 1"),
          nlocals("local K <const> = 10 return K + 1"),
          "both fold their constant away")

print("-- 3. Scope")
do
    const INNER = "in"
    assert_eq(INNER, "in", "visible in its block")
end
assert_eq(INNER, nil, "and gone after it")

const SHADOW = 1
do
    const SHADOW = 2
    assert_eq(SHADOW, 2, "an inner const shadows an outer one")
end
assert_eq(SHADOW, 1, "which is restored on the way out")

print("-- 4. The keyword's name outlives the collector")
-- See the note in test_continue.lua: a contextual keyword is compared by
-- string identity, so its name is fixed in 'luaX_init' alongside the
-- reserved words.
collectgarbage("collect")
for _ = 1, 200 do local churn = ("const"):sub(1, 5) .. tostring(_) end
collectgarbage("collect")
collectgarbage("collect")
const AFTER_GC = 11
assert_eq(AFTER_GC, 11, "'const' still declares")
assert_nocompile("const Q = 1; Q = 2", "and still refuses assignment")

print("-- 5. 'const' is still an ordinary name")
local const = 5
assert_eq(const, 5, "as a local")
const = 6
assert_eq(const, 6, "assigned to")
const = {f = function(self, x) return x end, g = 3, [1] = "idx"}
assert_eq(const:f(7), 7, "a method call")
assert_eq(const.g, 3, "a field")
assert_eq(const[1], "idx", "an index")
const.h = 4
assert_eq(const.h, 4, "assigning to a field")
local other
const, other = 1, 2
assert_eq(const + other, 3, "a multiple assignment")
local t = {const = 1}
assert_eq(t.const, 1, "as a table key")
local function callconst(f) return f(2) end
assert_eq(callconst(function(x) return x end), 2, "and unrelated calls are fine")

print("\n=== All Const Tests Passed ===")
