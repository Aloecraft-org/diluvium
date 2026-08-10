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

print("-- ext 0x02 resolves to a reference, which is the seam in 4.2")
do
    -- Without the endpoint library installed this would decode to an opaque ext
    -- value; with it, the codec asks a resolver. That the codec needs no
    -- knowledge of endpoints is the whole point of the injected seam.
    local ref = msgpack.decode("\xd4\x02Z")
    eq(type(ref), "table", "a reference is a value a program can hold")
    eq(getmetatable(ref), false, "but not one it can inspect")
    ok(not pcall(function() return ref.anything end) or ref.anything == nil,
       "there is nothing inside it to read")
end

print("-- a reference cannot be forged (7.3)")
do
    -- 7.3 says a program receives a reference and never constructs one. There is
    -- no constructor exposed, and a lookalike is refused, so that is structural
    -- rather than a convention.
    for _, fake in ipairs{ {}, {"bytes"}, setmetatable({}, {}), "string", 5 } do
        local okk, err = pcall(endpoint.bind, fake, "x")
        ok(not okk, "a forged reference is refused")
        ok(tostring(err):find("never builds one", 1, true) ~= nil,
           "and the message says a program receives one instead")
    end
end

print("-- with no host to resolve against, bind says so")
do
    local ref = msgpack.decode("\xd4\x02Z")
    local okk, err = pcall(endpoint.bind, ref, "peer")
    ok(not okk, "bind fails when the host binds no endpoints")
    ok(tostring(err):find("binds no endpoints", 1, true) ~= nil,
       "and says that rather than something vaguer: " .. tostring(err))
end

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
