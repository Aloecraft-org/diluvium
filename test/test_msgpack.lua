-- test_msgpack.lua
-- The round-trip corpus doc/Messaging.md 14 asks for, plus the boundaries
-- that decide whether the codec is right rather than merely working.
--
-- Two things here are worth knowing before editing:
--
-- Encoded bytes are asserted directly, not only round-tripped. A codec that
-- is wrong in both directions round-trips perfectly, so a round trip alone
-- proves interoperability with nothing. The hex literals are the wire format
-- from the msgpack spec, so a change that breaks another implementation
-- breaks these first.
--
-- Integer width boundaries are tested on both sides of each cutoff. That is
-- where a ladder of else-ifs goes wrong, and where a corpus of typical values
-- notices nothing.

local pass, fail = 0, 0

local function ok(cond, what)
    if cond then
        pass = pass + 1
    else
        fail = fail + 1
        print(string.format("[FAIL] %s", what))
    end
end

local function eq(actual, expected, what)
    if actual == expected then
        pass = pass + 1
    else
        fail = fail + 1
        print(string.format("[FAIL] %s\n  expected %s\n  actual   %s",
              what, tostring(expected), tostring(actual)))
    end
end

local function hex(s)
    return (s:gsub('.', function(c) return string.format('%02x', c:byte()) end))
end

local function bytes(v, expected, what)
    eq(hex(msgpack.encode(v)), expected, what or ("encodes as " .. expected))
end

local function rt(v)
    return msgpack.decode(msgpack.encode(v))
end

print("=== msgpack ===")

print("-- scalars, with their wire bytes")
bytes(nil, "c0", "nil")
bytes(false, "c2", "false")
bytes(true, "c3", "true")
eq(rt(nil), nil, "nil round-trips")
eq(rt(false), false, "false round-trips")
eq(rt(true), true, "true round-trips")

print("-- integers stay integers")
-- The property 13's acceptance criteria name: an integer must not come back a
-- float. In 5.3+ that is a real distinction and a codec without the integer
-- subtype silently loses it.
for _, v in ipairs{0, 1, -1, 127, 128, 255, 256, -32, -33, -128, -129,
                   32767, -32768, 65535, 2147483647, -2147483648} do
    eq(rt(v), v, "round-trips " .. v)
    eq(math.type(rt(v)), "integer", v .. " is still an integer")
end
eq(rt(math.maxinteger), math.maxinteger, "maxinteger round-trips")
eq(rt(math.mininteger), math.mininteger, "mininteger round-trips")
eq(math.type(rt(math.mininteger)), "integer", "mininteger is still an integer")

print("-- integer width boundaries, both sides of every cutoff")
bytes(127, "7f", "127 is the last positive fixint")
bytes(128, "cc80", "128 crosses to uint8")
bytes(255, "ccff", "255 is the last uint8")
bytes(256, "cd0100", "256 crosses to uint16")
bytes(65535, "cdffff", "65535 is the last uint16")
bytes(65536, "ce00010000", "65536 crosses to uint32")
bytes(4294967295, "ceffffffff", "4294967295 is the last uint32")
bytes(4294967296, "cf0000000100000000", "4294967296 crosses to uint64")
bytes(-32, "e0", "-32 is the last negative fixint")
bytes(-33, "d0df", "-33 crosses to int8")
bytes(-128, "d080", "-128 is the last int8")
bytes(-129, "d1ff7f", "-129 crosses to int16")
bytes(-32768, "d18000", "-32768 is the last int16")
bytes(-32769, "d2ffff7fff", "-32769 crosses to int32")
bytes(-2147483648, "d280000000", "-2147483648 is the last int32")
bytes(-2147483649, "d3ffffffff7fffffff", "-2147483649 crosses to int64")
bytes(math.mininteger, "d38000000000000000", "mininteger is int64")

print("-- floats are always float64")
-- 5.2 fixes the width by type rather than by magnitude, so that two equal
-- floats always encode identically. Upstream narrowed to float32 when the
-- value survived it, which makes the encoding depend on the value.
bytes(1.0, "cb3ff0000000000000", "1.0 is float64, not float32")
bytes(1.5, "cb3ff8000000000000", "1.5 is float64")
eq(math.type(rt(1.0)), "float", "a float comes back a float")
eq(rt(0.1), 0.1, "0.1 round-trips exactly")
eq(rt(-0.0), -0.0, "negative zero round-trips")
eq(rt(math.huge), math.huge, "infinity round-trips")
ok(rt(0/0) ~= rt(0/0), "NaN round-trips as a NaN")
-- float32 is accepted on decode even though it is never emitted, since other
-- implementations do emit it: 0xca 3f800000 is 1.0.
eq(msgpack.decode("\xca\x3f\x80\x00\x00"), 1.0, "float32 decodes")

print("-- strings, including the empty and embedded-zero cases")
bytes("", "a0", "the empty string is fixstr 0")
bytes("hi", "a26869", "a short string is fixstr")
eq(rt("a\0b"), "a\0b", "an embedded zero survives")
eq(rt(string.rep("x", 31)), string.rep("x", 31), "31 bytes round-trips")
eq(rt(string.rep("x", 32)), string.rep("x", 32), "32 bytes crosses to str8")
eq(hex(msgpack.encode(string.rep("x", 32))):sub(1, 4), "d920",
   "32 bytes uses str8")
eq(rt(string.rep("x", 255)), string.rep("x", 255), "255 bytes round-trips")
eq(hex(msgpack.encode(string.rep("x", 256))):sub(1, 6), "da0100",
   "256 bytes crosses to str16")
eq(#rt(string.rep("x", 70000)), 70000, "70000 bytes crosses to str32")
-- bin8/16/32 are not emitted, but must decode: other encoders use them for
-- byte strings, which Lua does not distinguish from text.
eq(msgpack.decode("\xc4\x02hi"), "hi", "bin8 decodes as a string")

print("-- array versus map, exactly as 5.3 states it")
bytes({}, "80", "the EMPTY TABLE IS A MAP, not an array")
bytes({1, 2, 3}, "93010203", "a dense 1..n sequence is an array")
bytes({a = 1}, "81a16101", "a string-keyed table is a map")
eq(hex(msgpack.encode({[1] = "a", [3] = "b"})):sub(1, 2), "82",
   "a sparse integer table is a map")
eq(hex(msgpack.encode({[0] = "a"})):sub(1, 2), "81",
   "a zero index makes it a map")
eq(hex(msgpack.encode({[-1] = "a"})):sub(1, 2), "81",
   "a negative index makes it a map")
eq(hex(msgpack.encode({[1.5] = "a"})):sub(1, 2), "81",
   "a float key makes it a map")
eq(hex(msgpack.encode({1, 2, extra = true})):sub(1, 2), "83",
   "a sequence with one extra key is a map (of three pairs)")
local big = {}
for i = 1, 16 do big[i] = i end
eq(hex(msgpack.encode(big)):sub(1, 6), "dc0010", "17+ elements uses array16")

print("-- forced shapes")
bytes(msgpack.as_array({1, 2}), "920102", "as_array on a sequence")
bytes(msgpack.as_map({1, 2}), "8201010202", "as_map forces a sequence to a map")
bytes(msgpack.as_array({}), "90", "as_array on an empty table gives array 0")
bytes(msgpack.as_map({}), "80", "as_map on an empty table gives map 0")
do
    local t = setmetatable({{1}, 1}, {})  -- a lookalike with a real metatable
    eq(hex(msgpack.encode(t)):sub(1, 2), "92",
       "a table that merely looks like a wrapper is not treated as one")
end
ok(not pcall(msgpack.as_array, 5), "as_array rejects a non-table")

print("-- nesting and cycles")
eq(rt({a = {b = {c = {1, 2}}}}).a.b.c[2], 2, "nested tables round-trip")
do
    local t, cur = {}, nil
    cur = t
    for _ = 1, 15 do cur.n = {}; cur = cur.n end
    ok(pcall(msgpack.encode, t), "nesting at the cap is accepted")
end
do
    local t = {}
    t.self = t
    local okk, err = pcall(msgpack.encode, t)
    ok(not okk, "a cyclic table raises rather than crashing or truncating")
    ok(tostring(err):find("cycle", 1, true) ~= nil,
       "and the message mentions a cycle")
end

print("-- non-encodable values name their type and their key path")
-- 5.6: this message is the main diagnostic a programmer gets when their state
-- is not serializable, so its content is asserted, not just its existence.
do
    local _, err = pcall(msgpack.encode, {a = {b = {print}}})
    err = tostring(err)
    ok(err:find("function", 1, true) ~= nil, "the offending type is named")
    ok(err:find("a.b[1]", 1, true) ~= nil,
       "and the key path locates it: " .. err)
end
do
    local _, err = pcall(msgpack.encode, print)
    ok(tostring(err):find("function", 1, true) ~= nil,
       "a bare function is refused too")
end
ok(not pcall(msgpack.encode, coroutine.create(function() end)),
   "a coroutine is refused")

print("-- decode offset")
do
    local s = msgpack.encode(11) .. msgpack.encode(22) .. msgpack.encode(33)
    eq(select("#", msgpack.decode(s)), 1,
       "decode with no offset returns one value")
    local v1, n1 = msgpack.decode(s, 1)
    local v2, n2 = msgpack.decode(s, n1)
    local v3 = msgpack.decode(s, n2)
    eq(v1, 11, "first value")
    eq(v2, 22, "second value, via the returned offset")
    eq(v3, 33, "third value")
    ok(not pcall(msgpack.decode, s, 0), "offset 0 is rejected")
    ok(not pcall(msgpack.decode, s, #s + 2), "an offset past the end is rejected")
end

print("-- the ext registry in 5.5")
do
    local e = msgpack.ext(0x20, "payload")
    local d = msgpack.decode(msgpack.encode(e))
    eq(d[3], 0x20, "an application ext round-trips its code")
    eq(d[1], "payload", "and its bytes")
    ok(not pcall(msgpack.ext, 0x01, "x"), "a reserved code cannot be built")
    ok(not pcall(msgpack.ext, 0x80, "x"), "nor one past the application range")

    local function err_of(s)
        local _, e2 = pcall(msgpack.decode, s)
        return tostring(e2)
    end
    ok(err_of("\xd4\x01x"):find("decQuad", 1, true) ~= nil,
       "ext 0x01 says decQuad specifically, not 'unknown ext'")
    for _, code in ipairs{0x03, 0x04, 0x05, 0x06, 0x07} do
        ok(err_of("\xd4" .. string.char(code) .. "x"):find("snapshot", 1, true)
           ~= nil, string.format("ext 0x%02x is refused as snapshot-only", code))
    end
    ok(err_of("\xd4\x09x"):find("reserved", 1, true) ~= nil,
       "an unassigned Diluvium code is refused by name")
    ok(err_of("\xd4\x09x"):find("0x09", 1, true) ~= nil,
       "and the message renders the code as hex")
    -- 4.2 has two halves, and only one of them is reachable from here.
    --
    -- With a resolver installed, ext 0x02 becomes an endpoint reference. This
    -- build has one, because 'luaopen_dendpoint' installs it -- so that is what
    -- this asserts, and the assertion changing is the resolver seam working
    -- rather than a regression.
    --
    -- The other half -- that with NO resolver the same bytes decode to an opaque
    -- ext value, so an embedder linking only the codec never needs one -- cannot
    -- be reached from a state that has the endpoint library. It is covered by
    -- bindings/js, whose codec has no resolver at all and asserts exactly that.
    local ep = msgpack.decode("\xd4\x02x")
    eq(type(ep), "table", "ext 0x02 resolves to an endpoint reference")
    eq(getmetatable(ep), false, "which a program cannot inspect")
    eq(ep[3], nil, "and which is not an ext wrapper any more")
    -- every ext width
    eq(msgpack.decode("\xd4\x20a")[1], "a", "fixext1")
    eq(msgpack.decode("\xd5\x20ab")[1], "ab", "fixext2")
    eq(msgpack.decode("\xd6\x20abcd")[1], "abcd", "fixext4")
    eq(msgpack.decode("\xd7\x20abcdefgh")[1], "abcdefgh", "fixext8")
    eq(msgpack.decode("\xc7\x03\x20abc")[1], "abc", "ext8")
    eq(#msgpack.decode(msgpack.encode(msgpack.ext(0x20, string.rep("z", 300))))[1],
       300, "ext16 round-trips")
end

print("-- malformed input is refused, never crashes")
do
    local function refused(s, what)
        ok(not pcall(msgpack.decode, s), what)
    end
    refused("", "the empty string")
    refused("\xc1", "0xc1, which msgpack leaves unused")
    refused("\xcd\x01", "a truncated uint16")
    refused("\x93\x01", "an array whose elements run out")
    refused("\x81\xa1", "a map whose key runs out")
    refused("\xa5ab", "a fixstr longer than the input")
    refused("\xcf\xff\xff\xff\xff\xff\xff\xff\xff",
            "a uint64 too large for a Lua integer")
    -- Every single byte on its own: none may crash the interpreter, and this
    -- is the cheap version of the fuzzing the loader gets.
    local crashed = 0
    for b = 0, 255 do
        local okk = pcall(msgpack.decode, string.char(b))
        if okk == nil then crashed = crashed + 1 end
    end
    eq(crashed, 0, "no single byte crashes the decoder")
    -- Truncations of a real encoding, which is where length prefixes bite.
    local full = msgpack.encode({a = {1, 2, "three"}, b = {c = 4.5}})
    for i = 1, #full - 1 do
        local okk = pcall(msgpack.decode, full:sub(1, i))
        if okk == nil then crashed = crashed + 1 end
    end
    eq(crashed, 0, "nor any truncation of a valid encoding")
end

print("-- map keys")
do
    eq(rt({[1.5] = "x"})[1.5], "x", "a float key round-trips")
    eq(rt({[true] = "x"})[true], "x", "a boolean key round-trips")
    ok(not pcall(msgpack.decode, "\x81\xc0\x01"), "a nil map key is refused")
end

print("-- encode takes exactly one value")
ok(not pcall(msgpack.encode), "encode with no argument is an error")
ok(not pcall(msgpack.encode, 1, 2), "encode with two arguments is an error")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
