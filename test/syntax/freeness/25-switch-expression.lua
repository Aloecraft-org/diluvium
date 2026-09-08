-- freeness: "switch" as an expression.
-- A name followed on the same line by another expression is a syntax
-- error in stock Lua, which is what "switch code" is there.
local code = 404
local label = switch code case 200: "ok" case 404: "missing" else "?"
return label
