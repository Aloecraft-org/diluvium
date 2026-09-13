-- test_numerals.lua
-- Verifies numeric literal separators and binary literals (syntax
-- proposals 3.5): the values they denote, the placement rule for '_',
-- and that every numeral stock Lua accepts still means what it meant.

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

print("=== Starting Numeral Tests ===\n")

print("-- 1. Separators are ignored")
assert_eq(1_000_000, 1000000, "decimal integer")
assert_eq(1_0.5_5, 10.55, "either side of the point")
assert_eq(1_0e1_0, 10e10, "mantissa and exponent")
assert_eq(0xFF_FF, 65535, "hexadecimal")
assert_eq(0x1_0p1_0, 0x10p10, "hex float with a binary exponent")
assert_eq(math.type(1_0), "integer", "a separated integer is still an integer")
assert_eq(math.type(1_0.0), "float", "and a separated float is still a float")

print("-- 2. A separator must sit between digits")
assert_nocompile("return 1_", "trailing '_'")
assert_nocompile("return 1__0", "doubled '_'")
assert_nocompile("return 0x_FF", "'_' straight after '0x'")
assert_nocompile("return 1e_5", "'_' straight after the exponent mark")
assert_nocompile("return 1._5", "'_' straight after the point")
assert_nocompile("return 0b_1", "'_' straight after '0b'")

print("-- 3. Binary literals")
assert_eq(0b0, 0, "zero")
assert_eq(0b1010_0110, 166, "with separators")
assert_eq(0B1111, 15, "uppercase 'B'")
assert_eq(0b101, 5, "value")
assert_eq(math.type(0b101), "integer", "always an integer")
assert_eq(0b111111 | 0b1000000, 0x7F, "usable in bitwise expressions")
-- 64 ones is -1, the same wrap-around a hex literal has, so the two
-- describe bit patterns the same way.
assert_eq(0b1111111111111111111111111111111111111111111111111111111111111111,
          0xFFFFFFFFFFFFFFFF, "wraps like a hex literal")

print("-- 4. Malformed binary literals")
assert_nocompile("return 0b", "no digits")
assert_nocompile("return 0b12", "a digit that is not 0 or 1")
assert_nocompile("return 0b1f", "a letter")
assert_nocompile("return 0b1.5", "a fractional part")

print("-- 5. Nothing stock Lua accepts changed")
assert_eq(0xFF, 255, "hex")
assert_eq(1e10, 10000000000.0, "exponent")
assert_eq(0x1p4, 16.0, "hex float")
assert_eq(.5, 0.5, "leading point")
assert_eq(tonumber("0b1"), nil, "'tonumber' is untouched: it knows no '0b'")
assert_eq(tonumber("1_0"), nil, "and no separator either")
local _1 = 7
assert_eq(_1, 7, "a name may still begin with '_'")
local a1_0 = 8
assert_eq(a1_0, 8, "and contain digits and '_'")

print("\n=== All Numeral Tests Passed ===")
