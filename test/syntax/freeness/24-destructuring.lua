-- freeness: destructuring.
-- "local {" and "local [" are syntax errors in stock Lua.
local cfg = {host = "h", port = 80}
local {host, port} = cfg
local [first, second] = {1, 2}
return host, port, first, second
