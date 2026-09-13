-- test_class.lua
-- Verifies classes (syntax proposals 5.1): the statement, '@', 'extends',
-- 'super', 'static', field defaults, and the metamethod copying that
-- makes an inherited '__tostring' actually fire.
--
-- The desugar is plain metatables, so most of what is checked here is
-- that a class produces the object a hand-written Lua class library
-- would have produced -- and that the two places Lua's own rules bite
-- (a raw metamethod lookup, a shared mutable default) are handled.

local function assert_eq(actual, expected, name)
    if actual == expected then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected: '%s'", tostring(expected)))
        print(string.format("       Actual:   '%s'", tostring(actual)))
        os.exit(1)
    end
end

local function assert_nocompile(src, want, name)
    local fn, err = load(src)
    if fn ~= nil then
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        os.exit(1)
    elseif want ~= nil and not tostring(err):find(want, 1, true) then
        print(string.format("[FAIL] %s", name))
        print(string.format("       Expected an error containing: '%s'", want))
        print(string.format("       Got: '%s'", tostring(err)))
        os.exit(1)
    else
        print(string.format("[PASS] %s", name))
    end
end

print("=== Starting Class Tests ===\n")

print("-- 1. A class, and what one instance is")
class Account
    balance = 0
    owner = "nobody"

    function new(owner)
        @owner = owner
    end

    function deposit(amt)
        @balance += amt
        return self
    end

    static function empty() = Account("nobody")

    function __tostring() = "Account(" .. @owner .. ":" .. @balance .. ")"
end

local a = Account("bob")
assert_eq(a.owner, "bob", "the constructor ran")
assert_eq(a.balance, 0, "and the field default with it")
assert_eq(a:deposit(5):deposit(6).balance, 11, "a method chain")
assert_eq(tostring(a), "Account(bob:11)", "a metamethod from the body")
assert_eq(Account.empty().owner, "nobody",
          "a static method takes no self and is called on the class")
assert_eq(Account.__name, "Account", "__name is the class's name")
assert_eq(dv.isa(a, Account), true, "dv.isa says what it is")
assert_eq(getmetatable(a), Account, "an instance's metatable is its class")
assert_eq(Account.__index, Account, "which is where its methods are found")

print("-- 2. '@' is 'self'")
class Reader
    function new(t) @t = t end
    function direct() return @t end          -- '@name' is 'self.name'
    function keyed() return @["t"] end       -- and '@[k]' is 'self[k]'
    function dotted() return @.t end         -- and '@.x' too
    function bare() return self == @ end     -- '@' alone is 'self'
    function viaself() return @:direct() end -- '@:m()' is 'self:m()'
end
local r = Reader("v")
assert_eq(r:direct(), "v", "'@name'")
assert_eq(r:keyed(), "v", "'@[k]'")
assert_eq(r:dotted(), "v", "'@.x'")
assert_eq(r:bare(), true, "'@' alone")
assert_eq(r:viaself(), "v", "'@:m()'")
-- '@name()' is 'self.name()', a field call with no self -- the same
-- distinction Lua already draws between 'obj.m()' and 'obj:m()'.
class Strict
    function new() end
    function m() return self end
    function fieldcall() return @m() end
end
assert_eq(Strict():fieldcall(), nil, "'@m()' is a field call, not a method one")
-- '@' works in any method, not only a class's.
local plain = {v = 7}
function plain:read() return @v end
assert_eq(plain:read(), 7, "'@' in an ordinary method")

print("-- 3. 'extends' and 'super'")
class Base
    kind = "base"
    function new(n) @n = n end
    function name() return "Base" .. @n end
end
class Middle extends Base
    function new(n) super(n) @mine = true end
    function name() return "Middle<" .. super.name() .. ">" end
end
class Leaf extends Middle
    function new(n) super(n) end
end
local leaf = Leaf(3)
assert_eq(leaf.n, 3, "the parent's constructor ran")
assert_eq(leaf.kind, "base", "and its field defaults")
assert_eq(leaf.mine, true, "and the middle's body")
assert_eq(leaf:name(), "Middle<Base3>", "'super.m()' calls the parent with self")
assert_eq(dv.isa(leaf, Base), true, "dv.isa walks the chain")
assert_eq(dv.isa(leaf, Middle), true, "every step of it")
assert_eq(dv.isa(leaf, Leaf), true, "including the class itself")
assert_eq(dv.isa(Account("x"), Base), false, "and says no when it should")
assert_eq(dv.isa(5, Base), false, "a value with no metatable is not an instance")
-- A class with no constructor of its own still builds, through the
-- parent's.
class Quiet extends Base end
assert_eq(Quiet(9).n, 9, "a class with no 'new' forwards to its parent")
assert_eq(Quiet(9).kind, "base", "and still gets the defaults")
-- The parent expression is evaluated once.
local made = 0
local function makebase() made = made + 1 return Base end
class Once extends makebase() end
assert_eq(made, 1, "the parent expression runs once")

print("-- 4. The metamethod copy, which is the gotcha this handles")
class Vec
    x = 0
    y = 0
    function new(x, y) @x = x @y = y end
    function __add(o) return Vec(@x + o.x, @y + o.y) end
    function __eq(o) return @x == o.x and @y == o.y end
    function __len() return 2 end
    function __tostring() return "(" .. @x .. "," .. @y .. ")" end
end
class Vec3 extends Vec
    z = 0
    function new(x, y, z) super(x, y) @z = z end
end
assert_eq(tostring(Vec(1, 2) + Vec(3, 4)), "(4,6)", "an operator from the body")
assert_eq(Vec(1, 2) == Vec(1, 2), true, "and '=='")
assert_eq(#Vec(1, 2), 2, "and '#'")
-- Lua finds a metamethod with a raw lookup on the metatable and does not
-- follow its '__index', so a child only has its parent's metamethods
-- because class creation copied them.
assert_eq(tostring(Vec3(1, 2, 3)), "(1,2)", "an inherited '__tostring' fires")
assert_eq(rawget(Vec3, "__tostring") ~= nil, true, "because it was copied in")
assert_eq(Vec3(1, 2, 3).z, 3, "and the child's own field is still there")

print("-- 5. Field defaults")
-- Per instance, not shared: the Python trap, named in the proposals.
class Bag
    items = {}
    tag = "t"
    function new() end
end
local b1, b2 = Bag(), Bag()
b1.items[#b1.items + 1] = "x"
assert_eq(#b1.items, 1, "a table default is written")
assert_eq(#b2.items, 0, "and the next instance gets its own")
assert_eq(b1.items == b2.items, false, "two instances, two tables")
-- A default may read the constructor's parameters, because the prologue
-- runs after the parameter list is in scope.
class Sized
    function new(n) @n = n end
end
assert_eq(Sized(4).n, 4, "a constructor parameter")
-- The parent's defaults run when 'super' does, so a parent and a child
-- that name the same field leave the parent's value.
class PDef
    shared = "parent"
    function new() end
end
class CDef extends PDef
    shared = "child"
    function new() super() end
end
assert_eq(CDef().shared, "parent",
          "the parent's default wins, because 'super' runs after the child's")
class CNoSuper extends PDef
    shared = "child"
    function new() end
end
assert_eq(CNoSuper().shared, "child", "and does not when 'super' is not called")

print("-- 6. Classes meet the rest of the language")
-- Inside a method, inside a loop, with the forms from A4 and A5.
class Series
    values = {}
    function new(t) @values = t end
    function evens()
        local out = {}
        for _, v in ipairs(@values) do
            out[#out + 1] = if v % 2 == 0 then v else -v
        end
        return out
    end
    function head() = @values[1:2]
end
local s = Series({1, 2, 3, 4})
assert_eq(table.concat(s:evens(), ","), "-1,2,-3,4", "an 'if' expression in a method")
assert_eq(table.concat(s:head(), ","), "1,2", "and a slice")
-- A class declared inside a method, with its own parent.
class Outer
    function new() @x = 1 end
    function spawn()
        class Inner extends Outer
            function new() super() @y = 2 end
        end
        return Inner()
    end
end
local inner = Outer():spawn()
assert_eq(inner.x .. "," .. inner.y, "1,2", "a class declared inside a method")
-- Declared in a loop, so the registers it takes are reused.
for k = 1, 3 do
    class Counted
        n = k
        function new() end
        function get() = @n
    end
    assert_eq(Counted():get(), k, "a class in a loop, iteration " .. k)
end

print("-- 7. What is refused, and what is left alone")
assert_nocompile("class A function new() super() end end", "was declared without",
                 "'super' without 'extends'")
assert_nocompile("class A extends B function m() super:x() end end",
                 "rather than 'super:m", "'super:m()' names the form to use")
assert_nocompile("class A extends B function m() super end end",
                 "either called or indexed", "a bare 'super'")
assert_nocompile("class A function m() end x = 1 end",
                 "comes before the first method", "a field after a method")
assert_nocompile("class A function m() end", "to close 'class'",
                 "an unterminated body names the line")
assert_nocompile("class A y = 1 static x = 2 end", "'function' or 'end' expected",
                 "'static' without 'function'")
assert_nocompile("return @x", "no 'self' in scope", "'@' with no self")
-- The names stay names. 'class', 'extends', 'static' and 'super' are
-- contextual, and 'class(x)', 'class.x' and 'class = 1' are stock Lua
-- programs that keep their meaning.
local class, extends, static, super = 1, 2, 3, 4
assert_eq(class + extends + static + super, 10, "all four are ordinary names")
local t = {class = "c", super = "s"}
assert_eq(t.class .. t.super, "cs", "and ordinary keys")
local function callable(x) return x * 2 end
do
    local class = callable
    assert_eq(class (21), 42, "'class (x)' is still a call")
end

print("\n=== All Class Tests Passed ===")
