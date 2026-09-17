-- freeness: spread in a call and in a constructor.
-- "..." followed by a name is a syntax error in stock Lua: two
-- expressions with no operator between them.
local rest = {2, 3}
local all = {1, ...rest}
return select("#", ...all)
