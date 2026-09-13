-- freeness: a destructuring pattern as a "for" variable.
-- "for" wants a name, and "{" is not one.
local rows = {{name = "a", n = 1}, {name = "b", n = 2}}
local i = 0
local out
for {name, n} in function() i = i + 1 return rows[i] end do out = name .. n end
return out
