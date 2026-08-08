-- test_secure_dump.lua
-- What a secure (~) function promises is that its strings are not in the
-- dump in the clear. 5.5 stores each distinct string once and refers back
-- to it by index, so that promise is a property of the whole chunk, not of
-- the secure function alone: a literal shared with ordinary code has one
-- stored copy, and it is stored wherever it was written first.
--
-- These tests check both halves -- that nothing leaks, and that everything
-- still round-trips once strings are hidden away from their own function.

local function assert_eq(a, e, n)
    if a == e then print(string.format("[PASS] %s", n))
    else print(string.format("[FAIL] %s\n  expected '%s'\n  actual   '%s'",
         n, tostring(e), tostring(a))); os.exit(1) end
end

-- Build the marker at runtime so this file's own source, which the runner
-- never dumps, is not what the search finds.
local function marker(tail) return "Dlv" .. "Secret" .. tail end

local function dumped(src, strip)
    local f = assert(load(src, "=secure_dump_test"))
    return string.dump(f, strip)
end
local function absent(bytes, needle, n)
    assert_eq(bytes:find(needle, 1, true) == nil, true, n)
end
local function present(bytes, needle, n)
    assert_eq(bytes:find(needle, 1, true) ~= nil, true, n)
end

print("=== Starting Secure Dump Tests ===\n")

print("-- 1. A literal shared with the enclosing chunk")
-- The enclosing function dumps its own constants before recursing into its
-- children, so this literal is always written from the non-secure side
-- first, whatever the source order.
local s1 = marker("Alpha")
local before = dumped(([[
    local tag = %q
    local ~function check() return %q end
    return check, tag
]]):format(s1, s1))
absent(before, s1, "not stored in the clear when the chunk uses it first")

local after = dumped(([[
    local ~function check() return %q end
    local tag = %q
    return check, tag
]]):format(s1, s1))
absent(after, s1, "nor when the secure function comes first in the source")

print("\n-- 2. A literal shared between siblings")
local s2 = marker("Beta")
for _, order in ipairs{
    {"local function a() return %q end", "~function b() return %q end"},
    {"~function a() return %q end", "local function b() return %q end"},
} do
    local src = (order[1] .. "\n" .. order[2] .. "\nreturn a, b"):format(s2, s2)
    absent(dumped(src), s2, "not stored in the clear, either sibling order")
end

print("\n-- 3. Round-trip with the string hidden away from its own function")
-- The non-secure side now reads a scrambled string it did not scramble.
local s3 = marker("Gamma")
local chunk = load(dumped(([[
    local tag = %q
    local ~function check() return %q end
    local function plain() return %q end
    return check, tag, plain
]]):format(s3, s3, s3)))
local check, tag, plain = chunk()
assert_eq(check(), s3, "the secure function returns its literal")
assert_eq(tag, s3, "the enclosing chunk's copy survives")
assert_eq(plain(), s3, "a non-secure function reads the hidden copy")
assert_eq(rawequal(check(), tag), true, "still one interned string after load")

print("\n-- 4. Long strings, both sides of the short-string boundary")
-- Above LUAI_MAXSHORTLEN the loader takes a different path for each of
-- scrambled, fixed-buffer and copied strings.
for _, len in ipairs{8, 39, 40, 41, 300} do
    local s = marker("Delta") .. string.rep("x", len)
    local src = ([[
        local tag = %q
        local ~function check() return %q end
        return check, tag
    ]]):format(s, s)
    local bytes = dumped(src)
    absent(bytes, s, ("length %d not stored in the clear"):format(len))
    local c, t = load(bytes)()
    assert_eq(c(), s, ("length %d round-trips"):format(len))
    assert_eq(t, s, ("length %d round-trips on the plain side"):format(len))
end

print("\n-- 5. Debug names")
local s5 = marker("Epsilon")
local src5 = ([[
    local ~function check()
        local %s = 1
        return %s
    end
    return check
]]):format("v_" .. s5, "v_" .. s5)
absent(dumped(src5), "v_" .. s5, "a secure function's local names are hidden")
assert_eq(load(dumped(src5))()(), 1, "and it still runs")
-- Stripping removes them altogether; the chunk must still load.
assert_eq(load(dumped(src5, true))()(), 1, "runs when stripped")

print("\n-- 6. Ordinary chunks are not scrambled")
-- The taint set must be exactly the secure functions' strings: scrambling
-- more than that would be a silent behaviour change for everyone else.
local s6 = marker("Zeta")
present(dumped(("local t = %q return t"):format(s6)),
        s6, "a chunk with no secure function stores strings in the clear")
local mixed = dumped(([[
    local ~function check() return %q end
    local unrelated = %q
    return check, unrelated
]]):format(marker("Eta"), s6))
present(mixed, s6, "an unrelated literal beside a secure function is untouched")
absent(mixed, marker("Eta"), "while the secure one is hidden")

print("\n-- 7. Repeats and back-references")
local s7 = marker("Theta")
local uses = {}
for i = 1, 20 do
    local secure = (i == 7 or i == 13) and "~" or ""
    uses[i] = ("%sfunction f%d() return %q end"):format(secure, i, s7)
end
local bytes7 = dumped(table.concat(uses, "\n"))
absent(bytes7, s7, "one shared copy, hidden because two of twenty are secure")
load(bytes7)()
for i = 1, 20 do
    assert_eq(_G["f" .. i](), s7, ("copy %d round-trips"):format(i))
end

print("\n-- 8. A corrupt size field is rejected before it is used")
-- The size field now carries the scramble flag in its low bit, so the
-- smallest legal encoding of a written string is (0+1)*2. A 1 there is
-- corrupt, and removing the bias from it would underflow to a huge length.
local good = dumped(("local ~function c() return %q end return c"):format(marker("Iota")))
assert_eq(type(load(good)), "function", "the unmodified dump loads")

local seen, rejected = {}, 0
for i = 1, #good do
    for _, b in ipairs{0, 1, 2, 3, 0x7f, 0xff} do
        local f, err = load(good:sub(1, i - 1) .. string.char(b) .. good:sub(i + 1))
        if f == nil then
            rejected = rejected + 1
            seen[tostring(err):match("%(([^)]+)%)$") or "?"] = true
        end
    end
end
assert_eq(rejected > 0, true, "single-byte corruption is caught, never crashes")
assert_eq(seen["invalid string size"], true, "and an under-biased size by name")

print("\n=== All Secure Dump Tests Passed ===")
