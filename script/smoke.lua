-- Diluvium smoke test.
--
-- Runs on a built artifact to answer one question: is this binary a working
-- Diluvium? It touches every construct the fork adds, so a platform where
-- one of them miscompiled says so here rather than after someone downloads
-- it.
--
-- Deliberately a file rather than a string of `-e` one-liners. Those have
-- to survive shell quoting, YAML quoting and, for the container builds, a
-- second shell inside `podman run` -- by which point `$"..."` and nested
-- quotes are more escape than program. A file has none of that, and every
-- platform can run the identical checks.
--
-- Fast by design: this runs under QEMU emulation on the ARM targets, where
-- anything loop-heavy would cost minutes. The suite proper is elsewhere.

local function check(name, ok, detail)
    if ok then
        print("  ok    " .. name)
    else
        print("  FAIL  " .. name .. (detail and ("  -- " .. detail) or ""))
        os.exit(1)
    end
end

print("Diluvium smoke test")
print("  " .. _VERSION)

-- Stock Lua still works; that is the whole promise.
check("lua arithmetic", 2 + 2 == 4)
check("lua strings", ("abc"):upper() == "ABC")
check("lua tables", #({1, 2, 3}) == 3)
check("lua closures", (function () local n = 0; return function () n = n + 1; return n end end)()() == 1)

-- Null coalescing.
check("?? on nil", (nil ?? "fallback") == "fallback")
check("?? passes through", (false ?? "fallback") == false, "?? tests nil, not falsity")

-- String interpolation, plain and with a format spec.
local who, n = "world", 42
check("f-string", $"hello {who}" == "hello world")
check("f-string spec", $"{n::%05d}" == "00042")
check("f-string nesting", $"outer {$"inner {who}"}" == "outer inner world")

-- Compound assignment.
local total = 10
total += 5
total *= 2
check("compound assignment", total == 30)
local port = nil
port ??= 8080
check("??= assigns only when nil", port == 8080)

-- Switch.
local hit = nil
switch 302 do
    case 200, 204 then hit = "ok"
    case 301, 302 then hit = "redirect"
    default hit = "other"
end
check("switch", hit == "redirect")

-- Safe navigation.
local cfg = {server = {port = 443}}
check("?. through a value", cfg?.server?.port == 443)
check("?. short-circuits", cfg?.missing?.port == nil)
check("?. with ??", (cfg?.missing?.port ?? 80) == 80)

-- Defer.
local order = {}
do
    defer order[#order + 1] = "deferred"
    order[#order + 1] = "body"
end
check("defer", table.concat(order, ",") == "body,deferred")

-- With.
local closed = false
with r = setmetatable({}, {__close = function () closed = true end}) do
    check("with binds", r ~= nil)
end
check("with closes", closed)

-- Secure functions, and that a shared literal is not in the dump.
local shared = "SmokeShared" .. "Literal"
local src = ([[
    local tag = %q;
    ~function secret() return %q end
    return secret, tag
]]):format(shared, shared)
local bytes = string.dump(assert(load(src, "=smoke")))
check("secure function runs", (assert(load(bytes))())() == shared)
check("secure function hides shared literals",
      bytes:find(shared, 1, true) == nil,
      "the literal appears in the dump in the clear")

-- Bytecode round trip.
local rt = assert(load(string.dump(assert(load("return 6 * 7")))))
check("bytecode round trip", rt() == 42)

print("smoke: all checks passed")
