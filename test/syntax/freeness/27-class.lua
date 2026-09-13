-- freeness: classes.
-- "class" followed by a name is two expressions with nothing between
-- them, which stock Lua refuses.
class Point
  x = 0
  function new(x) @x = x end
  function get() = @x
end
return Point(3):get()
