-- freeness: the "~function" form.
-- "~" is bitwise-not in stock Lua and cannot introduce a statement.
~function SecureAdd(a, b)
  return a + b
end
return SecureAdd(1, 2)
