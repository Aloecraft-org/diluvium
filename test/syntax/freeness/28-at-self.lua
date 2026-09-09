-- freeness: "@" for "self".
-- "@" is not a Lua token at all, so it is an error everywhere.
local counter = {n = 0}
function counter:bump() @n = @n + 1 return @n end
return counter:bump()
