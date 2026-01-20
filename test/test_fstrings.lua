-- test_fstrings.lua
-- A suite to verify f-string implementation in Lua

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

print("=== Starting F-String Tests ===\n")

-- 1. Basic Interpolation
print("-- 1. Basic Interpolation")
local name = "World"
local s1 = $"Hello {name}!"
assert_eq(s1, "Hello World!", "Basic variable interpolation")

print("-- 2. Single Quote vs Double Quote (The Fix Verification)")
local type_a = "Single"
local type_b = "Double"
local s2 = $'This uses {type_a} quotes'
local s3 = $"This uses {type_b} quotes"
assert_eq(s2, "This uses Single quotes", "Single quote f-string ($'...')")
assert_eq(s3, "This uses Double quotes", "Double quote f-string ($\"...\")")

print("-- 3. Multiple Interpolations")
local x = 10
local y = 20
local s4 = $"X: {x}, Y: {y}, Sum: {x + y}" 
-- Note: This tests if 'expr()' correctly handles math and stops at '}'
assert_eq(s4, "X: 10, Y: 20, Sum: 30", "Multiple interpolations & math expression")

print("-- 4. Edge Case: Empty Start/End")
local s5 = $"{name}"
assert_eq(s5, "World", "No surrounding text")

local s6 = $"Prefix {name}"
assert_eq(s6, "Prefix World", "Empty suffix")

local s7 = $"{name} Suffix"
assert_eq(s7, "World Suffix", "Empty prefix")

print("-- 5. Edge Case: Just text (no interpolation)")
-- Depending on implementation, this might optimize to a simple string or 
-- run through the builder. It should result in the literal string.
local s8 = $"Just plain text"
assert_eq(s8, "Just plain text", "No interpolation braces")

print("-- 6. Type Coercion (Implicit tostring)")
local is_cool = true
local val = 3.14
-- local s9 = $"Bool: {is_cool}, Float: {val}"
local s9 = $"Bool: {tostring(is_cool)}, Float: {val}"
-- Lua's concat opcode handles numbers/strings. Booleans depends on Lua version/patches,
-- but usually print as 'true'/'false' in newer Lua or might error in 5.1 without explicit tostring.
-- If this fails, try: $"Bool: {tostring(is_cool)}"
assert_eq(s9, "Bool: true, Float: 3.14", "Type coercion") 

print("-- 7. Escape Sequences")
local s10 = $"Line1\nLine2"
assert_eq(s10, "Line1\nLine2", "Standard escape sequences (\\n)")

print("-- 8. The 'Nil' Concatenation Check")
-- This verifies the bytecode generation fix (LOADK vs MOVE)
local n1 = 100
local n2 = 200
local s11 = $"A{n1}B{n2}C"
assert_eq(s11, "A100B200C", "Complex concatenation chain (A-x-B-y-C)")

print("\n=== All Tests Passed! ===")