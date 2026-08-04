-- Tests for obfuscated ("secure") functions: ~function
--
-- Secure functions XOR-scramble their bytecode and string constants when
-- dumped. This is obfuscation, not cryptography; these tests assert the
-- mechanics, not any security property:
--   * a secure function still executes normally
--   * its dump round-trips through load() and behaves identically
--   * string constants do not appear in plaintext in the dump
--   * everything lexically inside a secure function is covered too
--   * `local ~function` declares a local, not a global

print("=== secure function tests ===")

-- Plain-text search helper (no patterns).
local function contains(haystack, needle)
  return string.find(haystack, needle, 1, true) ~= nil
end

-- Test 1: basic secure function executes normally
~function SecureAdd(a, b)
  return a + b
end
assert(SecureAdd(5, 3) == 8)

-- Test 2: string constants survive execution
~function SecureGreet(name)
  local prefix = "Hello, "
  local suffix = "! Welcome to the encrypted zone."
  return prefix .. name .. suffix
end
assert(SecureGreet("Alice") == "Hello, Alice! Welcome to the encrypted zone.")

-- Test 3: `local ~function` is local, not global
do
  local ~function SecureSecret()
    return "MARKER_LOCAL_SECRET_9271"
  end
  assert(SecureSecret() == "MARKER_LOCAL_SECRET_9271")
  assert(rawget(_G, "SecureSecret") == nil,
         "local ~function leaked into _G")
end

-- Test 4: constants are hidden in the dump, and the dump round-trips
~function DumpMe()
  local secret = "MARKER_DIRECT_CONSTANT_5150"
  return secret
end

function PlainDumpMe()
  local visible = "MARKER_PLAIN_CONSTANT_5151"
  return visible
end

local secure_dump = string.dump(DumpMe, true)
local plain_dump = string.dump(PlainDumpMe, true)

-- Sanity: the search itself works -- a normal dump shows its constant.
assert(contains(plain_dump, "MARKER_PLAIN_CONSTANT_5151"),
       "plain dump should contain its constant in plaintext")
-- The secure dump must not.
assert(not contains(secure_dump, "MARKER_DIRECT_CONSTANT_5150"),
       "secure dump leaked a string constant")

local reloaded = assert(load(secure_dump))
assert(reloaded() == "MARKER_DIRECT_CONSTANT_5150",
       "secure dump did not round-trip")

-- Test 5: nested closures inside a secure function are covered
~function OuterSecure()
  local function inner()
    local deep = function() return "MARKER_DEEP_CONSTANT_5153" end
    return "MARKER_NESTED_CONSTANT_5152", deep
  end
  return inner
end

local a, deepf = OuterSecure()()
assert(a == "MARKER_NESTED_CONSTANT_5152" and
       deepf() == "MARKER_DEEP_CONSTANT_5153")

local outer_dump = string.dump(OuterSecure, true)
assert(not contains(outer_dump, "MARKER_NESTED_CONSTANT_5152"),
       "nested closure constant leaked from secure dump")
assert(not contains(outer_dump, "MARKER_DEEP_CONSTANT_5153"),
       "doubly nested closure constant leaked from secure dump")

local outer_reloaded = assert(load(outer_dump))
local b, deepg = outer_reloaded()()
assert(b == "MARKER_NESTED_CONSTANT_5152" and
       deepg() == "MARKER_DEEP_CONSTANT_5153",
       "nested secure dump did not round-trip")

-- Test 6: a secure function's dump also round-trips unstripped
-- (debug info is dumped in the clear by design; only code and string
-- constants are scrambled)
local unstripped = string.dump(DumpMe, false)
assert(not contains(unstripped, "MARKER_DIRECT_CONSTANT_5150"))
assert(assert(load(unstripped))() == "MARKER_DIRECT_CONSTANT_5150")

-- Test 7: normal functions are completely unaffected
function NormalFunction()
  return "visible_string"
end
assert(NormalFunction() == "visible_string")
assert(contains(string.dump(NormalFunction, true), "visible_string"))

print("=== all secure function tests passed ===")
