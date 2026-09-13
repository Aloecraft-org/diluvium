-- freeness: the "$" string prefix.
-- "$" is not a token in stock Lua, and a name may not begin with one.
local name = "world"
return $"hello {name}"
