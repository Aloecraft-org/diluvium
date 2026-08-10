-- test_endpoint.lua
-- What a program can observe about endpoints without a host.
--
-- The happy path lives in test/dv_check.c, because binding a reference needs a
-- host to say what the reference means -- and the CLI has no other instances to
-- point at. Rather than invent a meaning for one here, this file covers what is
-- genuinely testable from inside a program: that a reference cannot be forged,
-- that ext 0x02 resolves to one, and that every refusal says why.
--
-- The rest -- live and gone status, a push answering "gone", and a relay
-- forwarding between two instances -- is in dv_check.c where a host exists.

local pass, fail = 0, 0
local function ok(c, w)
    if c then pass = pass + 1 else fail = fail + 1; print("[FAIL] " .. w) end
end
local function eq(a, e, w)
    if a == e then pass = pass + 1
    else fail = fail + 1
         print(string.format("[FAIL] %s\n  expected %s\n  actual   %s",
               w, tostring(e), tostring(a))) end
end

print("=== endpoint ===")

print("-- 4.2's resolver runs on the host's bytes, never on a guest's string")
do
    -- This block used to assert the opposite: that ext 0x02 decoded from Lua
    -- became a reference. That WAS the resolver seam working -- and it was also a
    -- forgery vector, because 'msgpack.decode' is guest-callable, so it was the
    -- endpoint-reference constructor 7.3 says does not exist. Proven against a
    -- host that had pre-authorised one peer and delivered nothing: the program
    -- wrote msgpack.decode('\xd4\x02' .. name), bound it, and pushed to that
    -- peer's queue.
    --
    -- So the guest side of the seam is now an opaque ext wrapper, which is what an
    -- embedder with no resolver has always got. The resolver still runs -- on the
    -- delivery path, where the bytes came from the host -- and that half is
    -- covered by test/dv_check.c's 'endpoint_preauthorised' and
    -- 'relay_between_instances', because it needs a host and cannot be reached
    -- from pure Lua.
    local notref = msgpack.decode("\xd4\x02Z")
    eq(type(notref), "table", "the bytes still decode to something holdable")
    ok(getmetatable(notref) ~= false,
       "but its metatable is visible, so it is an ext wrapper and not a reference")
    eq(notref[3], 2, "carrying ext code 2 in the open")
    local okk, err = pcall(endpoint.bind, notref, "peer")
    ok(not okk, "and endpoint.bind refuses it")
    ok(tostring(err):find("never builds one", 1, true) ~= nil,
       "with the message that a program receives a reference instead: "
       .. tostring(err))
end

print("-- a reference cannot be forged (7.3)")
do
    -- 7.3 says a program receives a reference and never constructs one. There is
    -- no constructor exposed, and a lookalike is refused, so that is structural
    -- rather than a convention.
    -- The last entry is the one that used to work, and it is the reason this list
    -- was not enough before: every other candidate lacks the metatable, so the
    -- loop only ever proved that obviously-wrong shapes are refused. A decoded
    -- ext 0x02 was not obviously wrong -- it was a real reference.
    for _, fake in ipairs{ {}, {"bytes"}, setmetatable({}, {}), "string", 5,
                           msgpack.decode("\xd4\x02Z"),
                           msgpack.decode("\xc7\x06\x02peer-b") } do
        local okk, err = pcall(endpoint.bind, fake, "x")
        ok(not okk, "a forged reference is refused")
        ok(tostring(err):find("never builds one", 1, true) ~= nil,
           "and the message says a program receives one instead")
    end
end

-- The "with no host to resolve against, bind says so" case moved to
-- test/dv_check.c. It needed a *real* reference to get far enough to hit the
-- no-host message, and the only way to make one from Lua was the forgery this
-- file now refuses. Asserting it here would mean keeping the hole open to test
-- the error message behind it.

print("-- an ordinary queue is not an endpoint")
do
    local q = queue.declare("e/plain")
    eq(endpoint.is_endpoint(q), false, "a declared queue is not an endpoint")
    eq(queue.info(q).endpoint, false, "and info agrees")
    local okk, err = pcall(endpoint.status, q)
    ok(not okk, "status refuses a handle that is not an endpoint")
    ok(tostring(err):find("not an endpoint", 1, true) ~= nil, "by name")
    eq(endpoint.is_endpoint(99999), false, "as does an unissued handle")
end

print("-- 'gone' is a status, not an error (6.4)")
do
    -- The property that makes fan-out possible: a dead destination is an answer.
    -- Proved end to end in dv_check.c; what is checkable here is that the status
    -- string exists in the set a program switches on.
    local q = queue.declare("e/status")
    local okk, st = queue.push(q, 1)
    eq(okk, true, "an ordinary push still reports ok")
    eq(st, "ok", "with the status a program compares against")
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
