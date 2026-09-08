-- test_libm.lua
-- Verifies the embedded libm (numeric spec stage 1): that 'math.exp' and
-- its neighbours route through the vendored openlibm, that the exact
-- functions do not, and that nothing else about the 'math' table changed.
--
-- The bit-exactness across targets is test/numeric/corpus.lua, which is
-- diffed target to target. This file is about the routing: that it
-- happened, that it reached the right entries, and that it left the rest
-- alone.

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

local function bits(x)
    return ("%016x"):format(string.unpack("<I8", string.pack("<d", x)))
end

print("=== Starting Embedded libm Tests ===\n")

print("-- 1. The routing happened")
-- This argument is one where the vendored implementation and glibc
-- disagree in the last bit. Measured, not guessed: 1,695 of 32,000
-- sampled arguments differ, which is why the platform's libm cannot be
-- the answer to a cross-target question. On a build where the install
-- failed, this is the line that says so.
assert_eq(bits(math.exp(-20.505154639175259)), "3e155df6442ca92a",
          "math.exp answers the vendored value, not the platform's")
assert_eq(bits(math.exp(1.0)), "4005bf0a8b145769", "exp(1)")
assert_eq(bits(math.log(2.0)), "3fe62e42fefa39ef", "log(2)")
assert_eq(bits(math.sin(1.0)), "3feaed548f090cee", "sin(1)")
assert_eq(bits(math.cos(1.0)), "3fe14a280fb5068c", "cos(1)")
assert_eq(bits(math.tan(1.0)), "3ff8eb245cbee3a6", "tan(1)")
assert_eq(bits(math.asin(0.5)), "3fe0c152382d7366", "asin(0.5)")
assert_eq(bits(math.acos(0.5)), "3ff0c152382d7366", "acos(0.5)")
assert_eq(bits(math.atan(1.0)), "3fe921fb54442d18", "atan(1)")
assert_eq(bits(math.atan(1.0, -1.0)), "4002d97c7f3321d2", "atan(y, x) is atan2")

print("-- 2. Argument reduction, where two libms diverge most")
assert_eq(bits(math.sin(1e22)), "bfeb453ab76bf397", "sin of a huge argument")
assert_eq(bits(math.cos(1e300)), "bfe2699022adc4c1", "cos of a vast one")
assert_eq(bits(math.tan(1e22)), "bffa0f79c1b6b258", "tan too")

print("-- 3. The exact functions were left alone")
-- IEEE 754 fixes these, so every platform already agrees and routing
-- them could only add code that answered the same.
assert_eq(bits(math.sqrt(2.0)), "3ff6a09e667f3bcd", "sqrt is correctly rounded")
assert_eq(math.sqrt(4.0), 2.0, "and exact where the answer is")
assert_eq(math.floor(-1.5), -2, "floor")
assert_eq(math.ceil(-1.5), -1, "ceil")
assert_eq(math.abs(-1.5), 1.5, "abs")
assert_eq(math.fmod(5.5, 2.0), 1.5, "fmod")
assert_eq(select(2, math.modf(2.5)), 0.5, "modf")

print("-- 4. 'math.log' keeps its special cases")
-- lmathlib.c answers log(x, 2) with log2 and log(x, 10) with log10 so
-- that these are exact rather than a ratio of two logarithms. Dropping
-- that on the way through would change an answer programs rely on.
assert_eq(math.log(8, 2), 3.0, "log(8, 2) is exactly 3")
assert_eq(math.log(100, 10), 2.0, "log(100, 10) is exactly 2")
assert_eq(math.log(1024, 2), 10.0, "and log(1024, 2) exactly 10")
-- Base 3 is not special-cased, so it is the ratio, bit for bit.
assert_eq(bits(math.log(7, 3)), bits(math.log(7) / math.log(3)),
          "an arbitrary base goes through the ratio")
-- And this is what the base-2 case buys: the ratio form is inexact for
-- eight of the first sixty powers of two, and the special case is not
-- for any of them.
local ratio_wrong, special_wrong = 0, 0
for k = 1, 60 do
    local x = 2.0 ^ k
    if math.log(x) / math.log(2) ~= k + 0.0 then ratio_wrong = ratio_wrong + 1 end
    if math.log(x, 2) ~= k + 0.0 then special_wrong = special_wrong + 1 end
end
assert_eq(special_wrong, 0, "log(2^k, 2) is exactly k for every k up to 60")
assert_eq(ratio_wrong > 0, true, "which the ratio form is not")
assert_eq(math.log(math.exp(1)), 1.0, "log and exp still invert")

print("-- 5. The 'math' table's surface is unchanged")
-- Routing must not add an entry. 'pow', 'log10', 'sinh', 'cosh', 'tanh'
-- and 'atan2' exist only under LUA_COMPAT_MATHLIB, and a build without
-- it must not gain them by being routed.
local compat = (math.pow ~= nil)
for _, name in ipairs{"pow", "log10", "sinh", "cosh", "tanh", "atan2"} do
    assert_eq(math[name] ~= nil, compat,
              string.format("'math.%s' is present exactly when the "
                            .. "compatibility layer is", name))
end
for _, name in ipairs{"exp", "log", "sin", "cos", "tan", "asin", "acos",
                      "atan", "sqrt", "floor", "ceil", "abs", "fmod",
                      "modf", "ldexp", "frexp", "max", "min", "random",
                      "randomseed", "tointeger", "type", "ult"} do
    assert_eq(type(math[name]), "function",
              string.format("'math.%s' is still a function", name))
end
assert_eq(math.pi, 3.141592653589793, "math.pi")
assert_eq(math.maxinteger + 1, math.mininteger, "the integer limits")

print("-- 6. Errors are unchanged")
-- The wrappers are lmathlib.c's functions with one call swapped, so a
-- bad argument fails the same way it always did.
assert_eq(select(2, pcall(math.exp, "x")):find("number expected") ~= nil, true,
          "a non-number is refused by exp")
assert_eq(select(2, pcall(math.log, {})):find("number expected") ~= nil, true,
          "and by log")
assert_eq(math.exp(0/0) ~= math.exp(0/0), true, "NaN in, NaN out")
assert_eq(math.exp(math.huge), math.huge, "exp of infinity")
assert_eq(math.exp(-math.huge), 0.0, "and of negative infinity")
assert_eq(math.log(0.0), -math.huge, "log of zero")
assert_eq(math.log(-1.0) ~= math.log(-1.0), true, "log of a negative is NaN")

print("\n=== All Embedded libm Tests Passed ===")
