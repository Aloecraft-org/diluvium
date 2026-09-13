-- freeness: the switch statement.
-- "switch" is an ordinary name in stock Lua, so "switch x do" is a call
-- to a global followed by a "do" that has no place there.
local x = 2
switch x do
  case 1 then return "one"
  case 2 then return "two"
  default return "other"
end
