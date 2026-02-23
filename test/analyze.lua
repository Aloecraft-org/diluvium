--[[
  analyze_test.lua
  Exercises the major cases in analyze.c's report output.
  Run: luac --report analyze_test.lua
]]

-- 1. Plain global variable
VERSION = "1.0.0"

-- 2. Global function (closure detection via OP_SETTABUP + OP_CLOSURE)
function greet(name)
  return "hello, " .. name
end

-- 3. Method (first param = self, is_method should be true)
function MyObj:update(dt)
  self.x = self.x + dt
end

-- 4. Vararg function
function sum(...)
  local total = 0
  for _, v in ipairs({...}) do
    total = total + v
  end
  return total
end

-- 5. Returns a table with array and hash parts
--    array_size=3, hash_size should decode from B field
function make_config()
  return {
    "alpha", "beta", "gamma",   -- array part (3 entries)
    host = "localhost",         -- hash part
    port = 8080,
    debug = false,
  }
end

-- 6. Returns a table with closures inside (contains_closures = true)
function make_counter(start)
  local n = start
  return {
    increment = function() n = n + 1 end,
    decrement = function() n = n - 1 end,
    value     = function() return n end,
  }
end

-- 7. Returns result of a call (return_kind = RETURN_KIND_CALL)
function get_name()
  return tostring(42)
end

-- 8. Returns an upvalue (return_kind = RETURN_KIND_UPVALUE)
local _cached = "cached_value"
function get_cached()
  return _cached
end

-- 9. Null coalescing (OP_2Q — Diluvium extension)
function coalesce_test(x)
  local label = x ?? "default"
  return label
end

-- 10. Nested functions (should appear as separate FunctionInfo entries)
function outer()
  local function inner_a(x) return x * 2 end
  local function inner_b(x) return x + 1 end
  return inner_a(inner_b(10))
end