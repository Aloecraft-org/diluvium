-- freeness: the with statement.
-- "with a = x do" reads in stock Lua as a call to "with" followed by "=".
local function res(n) return setmetatable({}, {__close = function() end}), n end
with a = res("a") do
  return a
end
