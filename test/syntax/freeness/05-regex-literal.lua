-- freeness: the backtick regex literal.
-- Backtick is a lexical error in stock Lua ("unexpected symbol").
local r = `(\d+)-(\d+)`
return r.source
