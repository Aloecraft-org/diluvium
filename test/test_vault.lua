-- test_vault.lua
-- The vault mint (script/mint_vault.lua, build10 Part 4): a compiled
-- secure function holding name->value pairs. What it promises is exactly
-- what a secure function promises -- the pairs are not in the dump in the
-- clear or under any single-byte xor -- plus that the vault still WORKS,
-- because a vault that hides its contents from its caller too is just a
-- smaller file.
--
-- The literals sit in a table constructor inside the ~function. That shape
-- is asserted here rather than assumed: taintSecureStrings marks every
-- string a secure function contributes, and this is the test that notices
-- if a rebase ever narrows "contributes" to something that misses
-- constructor keys or values.

local pass, fail = 0, 0
local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end

local V = dofile("../script/mint_vault.lua")

-- Markers built at runtime, as test_secure_dump.lua does, so this file's
-- own source is not what a search finds.
local secret1 = "Dlv" .. "VaultSecretAlpha" .. "9x"
local secret2 = "Dlv" .. "VaultSecretBeta" .. "7q"
local keyname = "api" .. "_key_" .. "alpha"

local dump = V.mint({
    { name = keyname,  value = secret1 },
    { name = "second", value = secret2 },
})

ok(type(dump) == "string" and #dump > 0, "the mint produces a dump")

-- The functional half: the dump loads as a binary chunk and answers.
local get = assert(load(dump, "=vault", "b"))()
ok(get(keyname) == secret1, "the vault answers a name it holds")
ok(get("second") == secret2, "and a second one")
ok(get("absent") == nil, "a name it does not hold is nil, not an error")

-- The hiding half: values, in the clear and under every single-byte xor.
ok(dump:find(secret1, 1, true) == nil, "a value is not in the dump plain")
ok(not V.leaks(dump, secret1),
   "nor under any single-byte xor (the scramble has not regressed)")
ok(not V.leaks(dump, secret2), "the second value neither")

-- Names are literals inside the secure function too, so they are hidden
-- with the values -- the constructor-keys half of the taint assertion.
ok(dump:find(keyname, 1, true) == nil,
   "a vault NAME is hidden as well: constructor keys are tainted")

-- The tripwire trips: a plain (non-secure) build of the same table must
-- fail the same checks, proving the checks can fail at all.
local plain_src = V.generate({ { name = keyname, value = secret1 } })
                   :gsub("~function", "function", 1)
local plain = string.dump(assert(load(plain_src, "=plain", "t")), true)
ok(plain:find(secret1, 1, true) ~= nil,
   "the same vault WITHOUT ~ leaks, so the checks above can fail")

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1, true) end
print("OK")
