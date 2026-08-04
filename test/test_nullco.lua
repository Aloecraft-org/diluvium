-- Tests for the null-coalescing operator: a ?? b
--
-- Semantics: result is a unless a is nil, in which case it is b.
-- ?? short-circuits: b is evaluated only when a is nil (like and/or).
-- false is NOT nil: false ?? b yields false.
-- Precedence is the same as 'and' (tighter than 'or'), left-associative.

print("=== null coalescing tests ===")

-- basic values
assert((nil ?? "fallback") == "fallback")
assert((42 ?? "fallback") == 42)
assert(("" ?? "fallback") == "")
assert((0 ?? "fallback") == 0)

-- false is not nil: this is the whole reason ?? exists alongside 'or'
assert((false ?? "fallback") == false)
assert((false or "fallback") == "fallback")  -- contrast

-- nil on both sides
assert((nil ?? nil) == nil)
assert((nil ?? false) == false)

-- locals and table fields
local x = nil
assert((x ?? "dflt") == "dflt")
x = "set"
assert((x ?? "dflt") == "set")
local t = {a = 1, f = false}
assert((t.a ?? 99) == 1)
assert((t.missing ?? 99) == 99)
assert((t.f ?? 99) == false)

-- short-circuit: rhs must NOT be evaluated when lhs is not nil
do
  local calls = 0
  local function effect(v) calls = calls + 1 return v end
  assert((1 ?? effect(2)) == 1)
  assert(calls == 0, "?? evaluated its rhs eagerly")
  assert((false ?? effect(2)) == false)
  assert(calls == 0, "?? evaluated rhs for a false (non-nil) lhs")
  assert((nil ?? effect(3)) == 3)
  assert(calls == 1)
end

-- short-circuit guards side effects with errors
do
  local function boom() error("rhs must not run") end
  assert(("ok" ?? boom()) == "ok")
end

-- chaining (left-associative): (a ?? b) ?? c
assert((nil ?? nil ?? "third") == "third")
assert((nil ?? "second" ?? "third") == "second")
assert(("first" ?? nil ?? "third") == "first")
assert((nil ?? false ?? "third") == false)

-- chaining short-circuits through the whole chain
do
  local calls = 0
  local function effect(v) calls = calls + 1 return v end
  assert((1 ?? effect(2) ?? effect(3)) == 1)
  assert(calls == 0)
end

-- function calls as operands (results truncate to one value)
local function pair() return nil, "extra" end
local function one() return "one" end
assert((pair() ?? "fb") == "fb")     -- first result is nil
assert((one() ?? "fb") == "one")

-- rhs with its own jump lists (comparisons, and/or)
local n = nil
assert((n ?? (3 > 2)) == true)
assert((n ?? (2 > 3)) == false)
assert((n ?? (nil or "y")) == "y")

-- lhs with its own jump lists
assert(((n or false) ?? "fb") == false)

-- precedence: same as 'and', tighter than 'or'
-- a or b ?? c  parses as  a or (b ?? c)
assert((false or nil ?? "x") == "x")
assert((false or 1 ?? "x") == 1)
-- a and b ?? c  parses as  (a and b) ?? c  (equal priority, left assoc)
assert((true and nil ?? "x") == "x")

-- as call arguments and in constructors
local function id(v) return v end
assert(id(nil ?? 7) == 7)
local c = { nil ?? "a", 2 ?? "b" }
assert(c[1] == "a" and c[2] == 2)

-- in returns
local function ret(v) return v ?? "dflt" end
assert(ret(nil) == "dflt")
assert(ret(false) == false)
assert(ret(1) == 1)

-- assignment forms
local y
y = nil ?? 5
assert(y == 5)
local z = t.missing ?? t.a ?? "last"
assert(z == 1)

-- nested inside f-strings (both extensions together)
local w = nil
assert($"v={w ?? 'none'}" == "v=none")

-- deeper expressions: ?? under arithmetic via parens
assert(((nil ?? 2) + 3) == 5)
assert(((10 ?? 2) + 3) == 13)

-- register discipline: many live locals around a ?? expression
do
  local a1, a2, a3, a4, a5 = 1, 2, 3, 4, 5
  local r = nil ?? (a1 + a2 + a3 + a4 + a5)
  assert(r == 15)
  assert(a1 == 1 and a5 == 5)
end

-- upvalues as operands
do
  local up = nil
  local function get() return up ?? "unset" end
  assert(get() == "unset")
  up = false
  assert(get() == false)
end

-- ?? results feeding method calls and indexing
local obj = {list = nil}
local list = obj.list ?? {}
list[#list + 1] = "item"
assert(#list == 1)

-- loops: same expression evaluated repeatedly
do
  local calls = 0
  local function effect() calls = calls + 1 return calls end
  local acc = 0
  for i = 1, 3 do
    local v = (i % 2 == 0) and i or nil
    acc = acc + (v ?? effect())
  end
  assert(acc == 1 + 2 + 2 and calls == 2)  -- i=1: effect()=1; i=2: v=2; i=3: effect()=2
end

print("=== all null coalescing tests passed ===")
