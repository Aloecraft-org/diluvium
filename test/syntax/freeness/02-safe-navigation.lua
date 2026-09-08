-- freeness: the "?." family.
-- Stock Lua has no "?" token at all outside a string.
local t = { a = { b = 1 } }
return t?.a?.b
