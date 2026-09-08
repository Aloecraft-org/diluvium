-- freeness: "_" inside a numeral.
-- Stock Lua reads "1_000" as the number 1 touching a letter, which its
-- lexer forces into "malformed number".
return 1_000_000
