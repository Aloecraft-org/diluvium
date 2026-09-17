-- freeness: compact lambdas.
-- "|" has no unary form in stock Lua, so it cannot begin an expression
-- there, and "||" is not a token at all.
local double = |x| x * 2
local pair = |a, b| a + b
local thunk = || 42
return double(1) + pair(1, 2) + thunk()
