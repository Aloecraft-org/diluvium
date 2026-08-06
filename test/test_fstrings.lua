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

-- Compiling 'src' must fail; used for the stock-Lua compatibility checks.
local function assert_nocompile(src, name)
    local ok = load(src)
    if ok == nil then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
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
local s9 = $"Bool: {is_cool}, Float: {val}"
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

-- ---------------------------------------------------------------------
-- The f-string escape grammar is the same one plain strings use, so that
-- rewriting "..." as $"..." never changes what the escapes mean.
-- ---------------------------------------------------------------------
print("\n-- 9. Full escape grammar")
assert_eq($"A\x41B", "A" .. "\x41" .. "B", "hex escape (\\xXX)")
assert_eq($"A\65B", "A" .. "\65" .. "B", "decimal escape (\\ddd)")
assert_eq($"A\u{48}B", "A" .. "\u{48}" .. "B", "unicode escape (\\u{XXX})")
assert_eq($"A\z
           B", "AB", "whitespace-zapping escape (\\z)")
assert_eq($"\a\b\f\n\r\t\v", "\a\b\f\n\r\t\v", "control escapes")
assert_eq($"q\"q'q\\q", "q\"q'q\\q", "quote and backslash escapes")
assert_eq($"A\
B", "A\nB", "escaped newline continues the line")

-- Escapes must still resolve identically with a placeholder in the string,
-- i.e. the escape reader is shared across every piece, not just the first.
local esc = "X"
assert_eq($"\x41{esc}\102", "A" .. "X" .. "f", "escapes in every piece")

print("-- 10. Literal braces")
assert_eq($"a\{b\}c", "a{b}c", "escaped braces (\\{ and \\})")
assert_eq($"\{{esc}\}", "{X}", "escaped braces around a placeholder")
assert_eq($"a}b", "a}b", "an unescaped '}' outside a placeholder is literal")

print("-- 11. tostring semantics")
-- Decision: interpolation stringifies, so values that '..' rejects still work.
local nothing = nil
assert_eq($"v={nothing}", "v=nil", "nil interpolates")
assert_eq($"v={false}", "v=false", "boolean interpolates")
local withmeta = setmetatable({}, {__tostring = function() return "META" end})
assert_eq($"v={withmeta}", "v=META", "__tostring is honoured")
local plain = {}
assert_eq($"v={plain}", "v=" .. tostring(plain), "plain table interpolates")
assert_eq($"v={print}", "v=" .. tostring(print), "function interpolates")

print("-- 12. tostring resolution goes through _ENV, not the local scope")
do
    -- A local named 'tostring' must not change what an f-string means.
    local tostring = function() return "HIJACKED" end
    local v = true
    assert_eq($"{v}", "true", "a local 'tostring' does not capture interpolation")
    assert_eq(tostring(), "HIJACKED", "...and the local is otherwise untouched")
end
do
    -- Replacing it in the environment, however, must take effect: this is
    -- what lets a sandbox with its own _ENV control stringification.
    local real = tostring
    _ENV.tostring = function(a) return "<" .. real(a) .. ">" end
    local ok = $"{1}"
    _ENV.tostring = real
    assert_eq(ok, "<1>", "_ENV.tostring is what interpolation calls")
end

print("-- 13. Nesting")
local i = 1
assert_eq($"a{ $'b{i}' }c", "ab1c", "nested f-string, mismatched delimiters")
assert_eq($'a{ $"b{i}" }c', "ab1c", "nested f-string, outer single-quoted")
assert_eq($"a{ $'b{ $"c{i}d" }e' }f", "abc1def", "three levels of nesting")

print("-- 14. Expressions inside placeholders")
local t = {k = "v", n = {deep = "D"}}
assert_eq($"[{ t["k"] }]", "[v]", "string index containing no brace")
assert_eq($"[{ t["}"] or "none" }]", "[none]", "a '}' inside a string literal")
assert_eq($"[{ t.n.deep }]", "[D]", "chained field access")
assert_eq($"[{ string.rep("ab", 2) }]", "[abab]", "function call")
local function two() return "first", "second" end
assert_eq($"[{ two() }]", "[first]", "multiple returns truncate to one")
local function va(...) return $"[{ ... }]" end
assert_eq(va("only"), "[only]", "vararg truncates to one")
assert_eq($"{ i > 0 and "pos" or "neg" }", "pos", "and/or expression")

print("-- 15. Adjacent and degenerate placeholders")
local a, b = 1, 2
assert_eq($"{a}{b}", "12", "adjacent placeholders, no literal between")
assert_eq($"{a}{b}{a}{b}", "1212", "four adjacent placeholders")
assert_eq($"", "", "empty f-string")
assert_eq($"{a}", "1", "a single placeholder and nothing else")

print("-- 16. Stock-Lua compatibility of the shared escape reader")
-- '\{' is an f-string-only escape; in a plain string it stays invalid,
-- exactly as stock Lua has it.
assert_nocompile([[return "a\{b"]], "'\\{' is still invalid in a plain string")
assert_nocompile([[return "a\}b"]], "'\\}' is still invalid in a plain string")
assert_nocompile([[return "\q"]], "unknown escapes still rejected in strings")
assert_nocompile([[return $"\q"]], "unknown escapes rejected in f-strings too")
assert_eq("a\x41\66\u{43}", "aABC", "plain-string escapes unchanged")

print("-- 17. Malformed f-strings are syntax errors")
assert_nocompile([[return $"a{b"]], "unclosed placeholder")
assert_nocompile([[return $"a{}"]], "empty placeholder")
assert_nocompile([[return $"unterminated]], "unterminated f-string")
assert_nocompile("return $\"a\nb\"", "raw newline inside an f-string")
assert_nocompile([[return $ident]], "'$' must introduce a string literal")

print("\n=== All Tests Passed! ===")
