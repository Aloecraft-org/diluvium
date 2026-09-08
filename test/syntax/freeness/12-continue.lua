-- freeness: the continue statement.
-- "continue" is an ordinary name in stock Lua, so "continue end" is an
-- expression that is not a statement.
for i = 1, 3 do
  if i == 2 then continue end
end
