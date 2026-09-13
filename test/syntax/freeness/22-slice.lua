-- freeness: slicing.
-- A ":" inside an index is a syntax error in stock Lua: an index is one
-- expression and ":" cannot continue one.
local xs = {1, 2, 3, 4, 5}
local a = xs[2:4]
local b = xs[3:]
local c = xs[:2]
return #a + #b + #c
