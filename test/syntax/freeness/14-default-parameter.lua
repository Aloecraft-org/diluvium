-- freeness: default parameter values.
-- "=" inside a parameter list is a syntax error in stock Lua.
local function connect(host, port = 8080)
  return host, port
end
return connect("h")
