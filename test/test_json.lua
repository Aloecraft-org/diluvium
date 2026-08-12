-- test_json.lua
-- The 'json' guest library: a Lua value to JSON text and back.
--
-- Encoded output is asserted directly where it is unambiguous, because a codec
-- wrong in both directions round-trips perfectly. The decoder is the half that
-- reads bytes from outside, so its refusals are tested as hard as its
-- successes: a parser that accepts a malformed number decodes it to a wrong
-- value in silence, which is worse than a crash.

local pass, fail = 0, 0
local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end
local function eq(got, want, what)
    ok(got == want, string.format("%s (got %q, want %q)",
        what, tostring(got), tostring(want)))
end
local function refuses(s, what)
    ok(pcall(json.decode, s) == false, "decode refuses " .. (what or s))
end

-- ---- encode: unambiguous shapes --------------------------------------

eq(json.encode(nil), "null", "encode nil")
eq(json.encode(true), "true", "encode true")
eq(json.encode(false), "false", "encode false")
eq(json.encode(0), "0", "encode 0")
eq(json.encode(-42), "-42", "encode negative int")
eq(json.encode(2.5), "2.5", "encode float")
eq(json.encode("hi"), '"hi"', "encode string")
eq(json.encode("a\"\\\n\t\r\b\f"), '"a\\"\\\\\\n\\t\\r\\b\\f"',
   "encode escapes the seven")
eq(json.encode("\0\31"), '"\\u0000\\u001f"', "encode escapes control bytes")
eq(json.encode({1, 2, 3}), "[1,2,3]", "encode array")
eq(json.encode({}), "{}", "encode empty table is an object")
eq(json.encode({a = 1}), '{"a":1}', "encode object")
eq(json.encode({[1] = "x", [2] = "y"}), '["x","y"]', "encode dense int keys")

-- UTF-8 in a string passes through unescaped.
eq(json.encode("caf\u{e9}"), '"caf\u{e9}"', "encode leaves UTF-8 alone")

-- ---- encode: refusals -------------------------------------------------

ok(pcall(json.encode, {[1] = 1, foo = 2}) == false,
   "encode refuses mixed int and string keys")
ok(pcall(json.encode, {[2] = 1}) == false,
   "encode refuses a sparse array")
ok(pcall(json.encode, print) == false, "encode refuses a function")
ok(pcall(json.encode, 1/0) == false, "encode refuses inf")
ok(pcall(json.encode, 0/0) == false, "encode refuses nan")
do
    local cycle = {}; cycle.self = cycle
    ok(pcall(json.encode, cycle) == false, "encode refuses a cycle")
end

-- ---- decode: values ---------------------------------------------------

eq(json.decode("null"), nil, "decode null is nil")
eq(json.decode("  true  "), true, "decode tolerates surrounding space")
eq(json.decode("-42"), -42, "decode negative int")
eq(json.decode("2.5"), 2.5, "decode float")
eq(json.decode("1e3"), 1000.0, "decode exponent is a float")
eq(json.decode('"caf\\u00e9"'), "caf\u{e9}", "decode \\u escape")
eq(json.decode('"\\uD83D\\uDE00"'), "\u{1F600}", "decode a surrogate pair")
do
    local v = json.decode('{"a":[1,2,3],"b":{"c":true},"d":null}')
    ok(v.a[2] == 2 and v.b.c == true and v.d == nil, "decode a nested document")
end
-- an integer stays exact; a value past 64 bits becomes an inexact float
ok(math.type(json.decode("9007199254740993")) == "integer", "decode keeps ints exact")
ok(math.type(json.decode("1e309")) == "float", "decode overflows to float")

-- ---- decode: refusals -------------------------------------------------

refuses("", "the empty document")
refuses("{", "an unclosed object")
refuses("[1,2", "an unclosed array")
refuses("[1,]", "a trailing comma")
refuses('{"a"}', "an object member with no value")
refuses('{a:1}', "an unquoted key")
refuses("1 2", "trailing bytes after a value")
refuses("-", "a lone minus")
refuses("1.", "a dot with no fraction")
refuses("01", "a leading zero")
refuses("1e", "an exponent with no digits")
refuses('"\1"', "a control character in a string")
refuses('"\\uZZZZ"', "a bad \\u escape")
refuses('"\\uD83D"', "a lone high surrogate")
refuses('"unterminated', "a string with no closing quote")

-- depth is bounded rather than recursed into.
refuses(string.rep("[", 500) .. string.rep("]", 500), "nesting past the cap")

-- ---- round-trips ------------------------------------------------------

for _, v in ipairs({
    0, -1, 2.5, "", "hello", "a\nb\tc",
}) do
    eq(json.decode(json.encode(v)), v, "scalar round-trip: " .. tostring(v))
end
do
    local doc = {name = "bob", nums = {1, 2, 3}, flag = true,
                 nested = {x = {y = "z"}}}
    local back = json.decode(json.encode(doc))
    ok(back.name == "bob" and back.nums[3] == 3 and back.flag == true
       and back.nested.x.y == "z", "a document round-trips")
end
-- a large array survives encode and decode
do
    local big = {}
    for i = 1, 5000 do big[i] = i * 2 end
    local back = json.decode(json.encode(big))
    ok(#back == 5000 and back[5000] == 10000, "a 5000-element array round-trips")
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
