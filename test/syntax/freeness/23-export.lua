-- freeness: the export statement.
-- "export" is an ordinary name in stock Lua, so "export function" and
-- "export NAME" are two expressions with nothing between them.
export PORT = 8080
export function connect(url)
  return url
end
