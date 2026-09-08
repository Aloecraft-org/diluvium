-- freeness: the defer statement.
-- "defer" is an ordinary name in stock Lua; "defer f()" is two expressions
-- with no operator between them.
local function f() end
do
  defer f()
end
