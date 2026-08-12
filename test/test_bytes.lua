-- test_bytes.lua
-- The 'bytes' guest library: hex, base64 and base64url, both directions.
--
-- Known vectors first, round-trips second. A codec that is wrong in both
-- directions round-trips perfectly, so the base64 cases below are the RFC 4648
-- test vectors and the hex and base64url cases are checked against values a
-- second implementation produced -- a change that breaks another reader breaks
-- these first. The error cases matter as much as the happy path: a decoder
-- that silently skips a bad byte is a decoder that accepts two spellings of one
-- string, so every refusal is asserted.

local pass, fail = 0, 0

local function ok(cond, what)
    if cond then
        pass = pass + 1
    else
        fail = fail + 1
        print(string.format("[FAIL] %s", what))
    end
end

local function eq(got, want, what)
    ok(got == want, string.format("%s (got %q, want %q)",
        what, tostring(got), tostring(want)))
end

-- A raised error, caught, so a refusal is a pass and a silent accept a fail.
local function refuses(fn, ...)
    local args = {...}
    return function(what)
        ok(select(1, pcall(fn, table.unpack(args))) == false, what)
    end
end

-- ---- hex -----------------------------------------------------------------

eq(bytes.tohex(""), "", "tohex of the empty string")
eq(bytes.tohex("abc"), "616263", "tohex of 'abc'")
eq(bytes.tohex("\0\255\16"), "00ff10", "tohex spans the byte range")
eq(bytes.fromhex("616263"), "abc", "fromhex back to 'abc'")
eq(bytes.fromhex(""), "", "fromhex of the empty string")
eq(bytes.fromhex("DEADBEEF"), bytes.fromhex("deadbeef"),
   "fromhex accepts either case")

refuses(bytes.fromhex, "abc")("fromhex refuses an odd length")
refuses(bytes.fromhex, "zz")("fromhex refuses a non-hex digit")
refuses(bytes.fromhex, "6g")("fromhex refuses one bad nibble")

-- ---- base64 (RFC 4648 section 10) ----------------------------------------

eq(bytes.tobase64(""),       "",         "b64 ''")
eq(bytes.tobase64("f"),      "Zg==",     "b64 'f'")
eq(bytes.tobase64("fo"),     "Zm8=",     "b64 'fo'")
eq(bytes.tobase64("foo"),    "Zm9v",     "b64 'foo'")
eq(bytes.tobase64("foob"),   "Zm9vYg==", "b64 'foob'")
eq(bytes.tobase64("fooba"),  "Zm9vYmE=", "b64 'fooba'")
eq(bytes.tobase64("foobar"), "Zm9vYmFy", "b64 'foobar'")

eq(bytes.frombase64("Zg=="),     "f",      "unb64 'f'")
eq(bytes.frombase64("Zm8="),     "fo",     "unb64 'fo'")
eq(bytes.frombase64("Zm9vYmFy"), "foobar", "unb64 'foobar'")
eq(bytes.frombase64("Zm9vYmE="), "fooba",  "unb64 padded")
eq(bytes.frombase64("Zm9vYmE"),  "fooba",  "unb64 tolerates a missing pad")

refuses(bytes.frombase64, "Zm9v!!")("b64 refuses a character outside the alphabet")
refuses(bytes.frombase64, "Zm9vY")("b64 refuses a one-past-a-group length")
-- The '+' and '/' of standard base64 are not the url alphabet.
refuses(bytes.frombase64url, "++//")("b64url refuses standard '+' and '/'")

-- ---- base64url (the JWT / URL alphabet, no padding) ----------------------

-- 0xfb 0xef 0xff encodes to the two alphabet-specific characters in both
-- positions: '+' '/' become '-' '_'.
local hi = bytes.fromhex("fbefff")
eq(bytes.tobase64(hi),    "++//", "b64 uses '+' and '/'")
eq(bytes.tobase64url(hi), "--__", "b64url uses '-' and '_'")
eq(bytes.tobase64url("foobar"), "Zm9vYmFy", "b64url of 'foobar' (no +// here)")
eq(bytes.tobase64url("f"),  "Zg", "b64url drops padding")
eq(bytes.tobase64url("fo"), "Zm8", "b64url drops padding, two-char tail")
eq(bytes.frombase64url("Zg"), "f", "unb64url without padding")
eq(bytes.frombase64url("--__"), hi, "unb64url of the alphabet-specific chars")

-- ---- percent-encoding (RFC 3986) -----------------------------------------

eq(bytes.urlencode("aA0-_.~"), "aA0-_.~", "urlencode leaves unreserved alone")
eq(bytes.urlencode("a b"), "a%20b", "urlencode a space is %20, not '+'")
eq(bytes.urlencode("/?&="), "%2F%3F%26%3D", "urlencode reserved, upper-case hex")
eq(bytes.urlencode("caf\u{e9}"), "caf%C3%A9", "urlencode a UTF-8 byte pair")
eq(bytes.urldecode("a%20b"), "a b", "urldecode %20")
eq(bytes.urldecode("a+b"), "a+b", "urldecode leaves '+' literal (not form-decoding)")
eq(bytes.urldecode("caf%C3%A9"), "caf\u{e9}", "urldecode back to UTF-8")
eq(bytes.urldecode(bytes.urlencode("the/quick?brown=fox&x=1 2")),
   "the/quick?brown=fox&x=1 2", "url round-trip")
refuses(bytes.urldecode, "%zz")("urldecode refuses a non-hex escape")
refuses(bytes.urldecode, "abc%4")("urldecode refuses a truncated escape")

-- ---- round-trips over the byte range -------------------------------------

local corpus = {
    "", "a", "ab", "abc", "abcd", "abcde",
    "\0", "\0\0\0", "\255\254\253", "\1\2\3\4\5\6\7\8",
    "the quick brown fox jumps over the lazy dog",
}
for _, s in ipairs(corpus) do
    eq(bytes.fromhex(bytes.tohex(s)), s, "hex round-trip of length " .. #s)
    eq(bytes.frombase64(bytes.tobase64(s)), s, "b64 round-trip of length " .. #s)
    eq(bytes.frombase64url(bytes.tobase64url(s)), s,
       "b64url round-trip of length " .. #s)
end

-- Every 256-byte value, in one string, through each codec.
local all = {}
for i = 0, 255 do all[#all + 1] = string.char(i) end
all = table.concat(all)
eq(bytes.fromhex(bytes.tohex(all)), all, "hex round-trip of all 256 byte values")
eq(bytes.frombase64(bytes.tobase64(all)), all, "b64 round-trip of all 256")
eq(bytes.frombase64url(bytes.tobase64url(all)), all, "b64url round-trip of all 256")

-- ---- cross-alphabet interop with the host's JWT MAC ----------------------

-- A JWT signature is base64url of raw HMAC bytes. A guest that can hex-decode
-- and base64url-encode can construct and read the token pieces the crypto
-- connector mints -- the gap this library exists to close. Prove the two
-- encodings compose the way a token assembler needs.
local raw = bytes.fromhex("00112233445566778899aabbccddeeff")
eq(bytes.frombase64url(bytes.tobase64url(raw)), raw,
   "raw MAC bytes survive a base64url round-trip")
eq(bytes.tohex(bytes.frombase64url(bytes.tobase64url(raw))),
   "00112233445566778899aabbccddeeff",
   "and back to the hex a guest would print")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
