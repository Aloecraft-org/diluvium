-- test_analysis.lua
-- Example file for testing the luac analyzer

-- Function that returns a table with closures
function my_table_function()
  local t = {}
  t.abc = function() return 1 end
  t.def = function() return 2 end
  t.ghi = function() return 3 end
  t.jkl = function() return 4 end
  return t
end

-- Regular function with parameters
function my_main_function(tbl)
  return tbl.abc() + tbl.def()
end

-- Function with varargs
function vararg_function(x, y, ...)
  local args = {...}
  return x + y + #args
end

-- Function that creates and returns a large table
function create_config()
  local config = {
    host = "localhost",
    port = 8080,
    timeout = 30,
    retry_count = 3,
    endpoints = {
      api = "/api/v1",
      health = "/health",
      metrics = "/metrics"
    },
    features = {
      auth = true,
      logging = true,
      monitoring = false
    }
  }
  return config
end

-- Function with upvalues
local counter = 0
function increment()
  counter = counter + 1
  return counter
end

-- Nested function with closures
function create_counter()
  local count = 0
  
  return {
    inc = function() count = count + 1 end,
    dec = function() count = count - 1 end,
    get = function() return count end
  }
end

-- Global variable assignment
MY_CONSTANT = 42
ANOTHER_VAR = "hello"