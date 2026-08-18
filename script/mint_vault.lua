-- mint_vault.lua
-- Mint a secrets vault: a compiled secure function (build10 Part 4).
--
--   VAULT_API_KEY=... SMTP_PASS=... \
--     diluvium script/mint_vault.lua vault.luac VAULT_API_KEY SMTP_PASS
--
-- Each name on the command line names an ENVIRONMENT VARIABLE; the values
-- never ride argv, where any process listing would read them. The output
-- is a stripped dump of
--
--   local ~function vault_get(name) ... end return vault_get
--
-- with the pairs as literals inside the secure function, so the scramble
-- (ldump.c's keystream) covers names and values wherever the chunk stores
-- them. A program that holds the bytes does
--
--   local get = load(bytes, "=vault", "b")()
--   local key = get("VAULT_API_KEY")
--
-- 'load' is available to sealed guests -- it compiles bytes the program
-- already holds and reaches nothing -- so the bytes travel any granted
-- channel: host.fs.read under an fs scope, or baked into a spawn's code.
--
-- The honest claim, in the ROADMAP's own words: obfuscation, not
-- encryption -- Kubernetes-secrets parity, not a vault appliance. Two
-- caveats are part of the tool's contract, not footnotes:
--
--   * SNAPSHOTS ARE THE LEAK THAT MATTERS. A secret fetched into a
--     retained local rides every hibernation snapshot in the clear.
--     Fetch per use; better, keep secrets that never need to enter the
--     sandbox out of it entirely (crypto.key_env today, rest credential
--     injection when it lands).
--   * A GUEST-SIDE VAULT IS ALL-OR-NOTHING. Whoever can call the
--     function holds every key. Per-key attenuation is the host-side
--     vault connector, recorded as the v2 direction in doc/BUILD10.md.
--
-- Before writing anything the tool verifies its own output: every value
-- must be absent from the dump in the clear and under all 256 single-byte
-- xor keys, the regression tripwire test_secure_dump.lua established. A
-- vault that fails that check is not written at all.

local M = {}

-- The generated source. Everything secret is a literal inside the secure
-- function, table-constructor keys included: the taint pass marks every
-- string a secure function contributes, and test_vault.lua asserts that
-- holds for this exact shape rather than assuming it.
function M.generate (entries)
  local buf = { "local ~function vault_get(name)\n  local t = {\n" }
  for _, e in ipairs(entries) do
    buf[#buf + 1] = ("    [%q] = %q,\n"):format(e.name, e.value)
  end
  buf[#buf + 1] = "  }\n  return t[name]\nend\nreturn vault_get\n"
  return table.concat(buf)
end

-- entries: an array of {name=..., value=...}. Returns the stripped dump.
function M.mint (entries)
  for _, e in ipairs(entries) do
    if type(e.name) ~= "string" or #e.name == 0 then
      error("mint_vault: every entry needs a non-empty name", 2)
    end
    if type(e.value) ~= "string" then
      error(("mint_vault: '%s' needs a string value"):format(e.name), 2)
    end
  end
  local src = M.generate(entries)
  local chunk = assert(load(src, "=vault", "t"))
  return string.dump(chunk, true)
end

-- Does 'value' appear in 'dump', in the clear or under any single-byte
-- xor? Key 0 is the plaintext case. This cannot prove the keystream
-- strong -- nothing this cheap could -- but it fails loudly if the
-- scramble ever regresses to the constant it once was.
function M.leaks (dump, value)
  if #value == 0 then return false end
  local byte, char, concat = string.byte, string.char, table.concat
  for key = 0, 255 do
    local x = {}
    for i = 1, #value do
      x[i] = char(byte(value, i) ~ key)
    end
    if dump:find(concat(x), 1, true) ~= nil then
      return true
    end
  end
  return false
end

local function main ()
  local out = arg[1]
  if out == nil or arg[2] == nil then
    io.stderr:write(
      "usage: diluvium script/mint_vault.lua OUT.luac NAME [NAME...]\n" ..
      "  each NAME names an environment variable holding that secret\n")
    os.exit(1, true)
  end
  local entries = {}
  for i = 2, #arg do
    local name = arg[i]
    local value = os.getenv(name)
    if value == nil or value == "" then
      io.stderr:write(("mint_vault: the environment variable '%s' is " ..
                       "unset or empty\n"):format(name))
      os.exit(1, true)
    end
    entries[#entries + 1] = { name = name, value = value }
  end
  local dump = M.mint(entries)
  for _, e in ipairs(entries) do
    if M.leaks(dump, e.value) then
      io.stderr:write(("mint_vault: REFUSING to write: the value of '%s' " ..
                       "is recoverable from the dump -- the secure-" ..
                       "function scramble has regressed\n"):format(e.name))
      os.exit(1, true)
    end
  end
  local f = assert(io.open(out, "wb"))
  assert(f:write(dump))
  f:close()
  io.write(("%s: %d bytes, %d entr%s (values checked, never printed)\n")
           :format(out, #dump, #entries, (#entries == 1) and "y" or "ies"))
end

-- Run directly: mint. Loaded by the test suite: hand over the functions.
if arg ~= nil and arg[0] ~= nil and arg[0]:find("mint_vault", 1, true) then
  main()
end

return M
