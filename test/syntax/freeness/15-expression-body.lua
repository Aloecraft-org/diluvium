-- freeness: expression-bodied functions.
-- After ")" a block begins, and "=" cannot start a statement.
local function area(w, h) = w * h
return area(3, 4)
