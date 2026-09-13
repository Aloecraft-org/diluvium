-- freeness: "if" as an expression.
-- "if" cannot begin an expression in stock Lua.
local sign = if 1 < 0 then -1 elseif 1 > 0 then 1 else 0
return sign
