-- test_tierb.lua
-- Verifies the Tier B syntax that landed (syntax proposals 4): compact
-- lambdas (4.1), destructuring (4.2), 'if' and 'switch' as expressions
-- (4.3), slicing (4.4) and 'export' (4.5).
--
-- One file rather than six because these forms meet each other
-- constantly -- a lambda whose body is an 'if' expression over a slice
-- is the shape they were added for -- and a file per form would test
-- each alone and none of them together.

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

local function assert_nocompile(src, name)
    local fn, err = load(src)
    if fn == nil then
        print(string.format("[PASS] %s (%s)", name, err))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        os.exit(1)
    end
end

print("=== Starting Tier B Syntax Tests ===\n")

print("-- 1. Compact lambdas")
assert_eq((|x| x * 2)(21), 42, "one parameter")
assert_eq((|a, b| a + b)(3, 4), 7, "two")
assert_eq((|| 42)(), 42, "none")
local xs = {3, 1, 2}
table.sort(xs, |a, b| a < b)
assert_eq(table.concat(xs, ","), "1,2,3", "as an argument")
-- '|x| x | y' is a lambda returning the bitwise or, as it reads: the
-- body is an expression and '|' inside it is the operator it always was.
assert_eq((|x, y| x | y)(1, 2), 3, "a '|' inside the body is bitwise or")
assert_eq(1 | 2, 3, "and '|' outside one still is too")
assert_eq((|x| (|y| x + y))(10)(5), 15, "nested, closing over the outer")
local n = 7
assert_eq((|x| x + n)(1), 8, "closing over a local")
-- The body is one expression, so it ends where the expression does.
local t = {half = |x| x / 2, name = "half"}
assert_eq(t.name, "half", "the comma after a lambda belongs to the table")
assert_eq(t.half(10), 5.0, "and the lambda is still the lambda")

print("-- 2. 'if' as an expression")
local function sign(x) return if x < 0 then -1 elseif x > 0 then 1 else 0 end
assert_eq(sign(-5) .. "," .. sign(0) .. "," .. sign(5), "-1,0,1", "three branches")
assert_eq(if true then "yes" else "no", "yes", "the simple form")
assert_eq(if false then 1 else if true then 2 else 3, 2, "nested in the else")
assert_eq(math.max(if true then 10 else 0, 5), 10, "as an argument")
-- The trap it exists to end: 'a and b or c' is wrong when b is false.
local flag = true
assert_eq(flag and false or "wrong", "wrong", "'and/or' picks the wrong branch")
assert_eq(if flag then false else "wrong", false, "and the expression does not")
assert_nocompile("return if true then 1", "'else' is required")
assert_eq((function() if true then return "statement" end end)(), "statement",
          "'if' is still a statement")

print("-- 3. Slicing")
local list = {10, 20, 30, 40, 50}
assert_eq(table.concat(list[2:4], ","), "20,30,40", "both ends")
assert_eq(table.concat(list[3:], ","), "30,40,50", "an open end")
assert_eq(table.concat(list[:2], ","), "10,20", "an open start")
assert_eq(table.concat(list[:], ","), "10,20,30,40,50", "both open")
assert_eq(table.concat(list[-2:], ","), "40,50", "a negative index counts back")
assert_eq(#list[4:2], 0, "a backwards range is empty")
assert_eq(table.concat(list, ","), "10,20,30,40,50", "and the source is unchanged")
local s = "hello world"
assert_eq(s[1:5], "hello", "a string slices like string.sub")
assert_eq(s[7:], "world", "with an open end")
assert_eq(s[-5:], "world", "and a negative index")
assert_eq(list[2], 20, "an ordinary index still indexes")
assert_eq(({a = 1})["a"], 1, "including a string key")
assert_eq(list[1:3][2], 20, "a slice can be indexed")
assert_eq(#list[2:4], 3, "and measured")
-- In an argument list, where a wrong register count would show up as a
-- phantom argument rather than a wrong value.
assert_eq(table.concat({"a", table.concat(list[2:3], "-"), "b"}, "|"),
          "a|20-30|b", "in the middle of an argument list")
-- '__slice' is what an object uses to say what a range of it means.
local counted = setmetatable({}, {__slice = function(_, i, j)
    return tostring(i) .. ":" .. tostring(j)
end})
assert_eq(counted[2:5], "2:5", "a __slice metamethod wins")
assert_eq(counted[2:], "2:nil", "and sees a missing end as nil")
assert_eq(counted[:5], "nil:5", "and a missing start")
-- Slicing something that has no ranges is an error naming the type, not
-- a quietly empty result. The message comes from 'dv.slice' and names the
-- type it got, so it differs from the index error the same value would
-- raise under 'q[1]'.
assert_eq(select(2, pcall(function() local q = nil return q[1:2] end))
          :find("cannot slice a nil value") ~= nil, true,
          "slicing nil says so")
assert_eq(select(2, pcall(function() return (5)[1:2] end))
          :find("cannot slice a number value") ~= nil, true,
          "and slicing a number names the type")

print("-- 4. 'export'")
-- Written to a file, because what 'export' does is decide what a *chunk*
-- returns, and a chunk is the unit being tested.
local path = os.tmpname()
local fh = assert(io.open(path, "w"))
fh:write([[
export PORT = 8080
export function connect(url) return url .. ":" .. PORT end
export function fact(n) return if n <= 1 then 1 else n * fact(n - 1) end
local hidden = "not exported"
]])
fh:close()
local m = assert(dofile(path))
os.remove(path)
assert_eq(type(m), "table", "a chunk that exports returns a table")
assert_eq(m.PORT, 8080, "a value export")
assert_eq(m.connect("h"), "h:8080", "a function export, seeing another")
assert_eq(m.fact(5), 120, "and calling itself")
assert_eq(m.hidden, nil, "a local is not exported")
assert_eq(connect, nil, "and an export is not a global")
assert_nocompile("export A = 1 return 2", "an explicit return is refused")
assert_nocompile("local function f() export A = 1 end", "not inside a function")
assert_nocompile("do export A = 1 end", "not inside a block")

print("-- 5. Destructuring")
local cfg = {host = "h", port = 80, tls = true}
local {host, port} = cfg
assert_eq(host .. ":" .. port, "h:80", "keyed, with braces")
local [first, second] = {"a", "b", "c"}
assert_eq(first .. second, "ab", "positional, with brackets")
local calls = 0
local function source() calls = calls + 1 return {x = 1, y = 2} end
local {x, y} = source()
assert_eq(x + y, 3, "both names bound")
assert_eq(calls, 1, "from one evaluation of the source")
local {absent} = cfg
assert_eq(absent, nil, "a missing field is nil, not an error")
-- The source is evaluated before any name enters scope, so a pattern may
-- name the variable it is reading from.
local shadow = "outer"
do
    local {shadow} = {shadow = "inner"}
    assert_eq(shadow, "inner", "a pattern may shadow its own source")
end
assert_eq(shadow, "outer", "and the outer one is untouched")
for i = 1, 2 do
    local {each} = {each = i}
    assert_eq(each, i, "a pattern inside a loop, iteration " .. i)
end
assert_nocompile("local {} = {}", "an empty pattern is refused")
assert_eq(({1, 2})[1], 1, "a table constructor is untouched")
assert_eq(#{3, 4, 5}, 3, "and so is its length")
-- The same pattern as a loop variable, where the source is whatever the
-- iterator hands back each step.
local people = {{name = "ada", age = 36}, {name = "bob", age = 41}}
local seen = {}
local k = 0
for {name, age} in function() k = k + 1 return people[k] end do
    seen[#seen + 1] = name .. ":" .. age
end
assert_eq(table.concat(seen, ","), "ada:36,bob:41", "a pattern as a loop variable")
local pairs2 = {{1, 2}, {3, 4}}
local j = 0
local pairsum = 0
for [a, b] in function() j = j + 1 return pairs2[j] end do
    pairsum = pairsum + a * b
end
assert_eq(pairsum, 14, "a positional pattern as a loop variable")
local rounds = 0
for {v} in pairs({}) do rounds = rounds + 1 end
assert_eq(rounds, 0, "a loop that never runs binds nothing")
assert_nocompile("for {} in f() do end", "an empty loop pattern is refused")
assert_nocompile("for {a}, k in f() do end", "one pattern and no extra names")

print("-- 6. 'switch' as an expression")
local code = 404
assert_eq(switch code case 200: "ok" case 404: "missing" else "?", "missing",
          "an arm in the middle")
assert_eq(switch 1 case 1, 2: "low" case 3: "high" else "?", "low",
          "an arm matching either of two")
assert_eq(switch 9 case 1: "a" else "fallback", "fallback", "the 'else' arm")
assert_eq(switch 3 else "no arms at all", "no arms at all", "no arms at all")
-- The subject is evaluated once, whichever arm wins.
local subjcalls = 0
local function subject() subjcalls = subjcalls + 1 return 3 end
assert_eq(switch subject() case 1: "a" case 2: "b" case 3: "c" else "d", "c",
          "a matching arm past the first")
assert_eq(subjcalls, 1, "and the subject was evaluated once")
-- In an argument list, where a wrong register count shows up as a
-- phantom argument rather than a wrong value.
local packed = table.pack("a", switch code case 404: "b" else "z", "c")
assert_eq(packed.n .. table.concat(packed), "3abc", "in the middle of an argument list")
assert_eq(math.max(switch 1 case 1: 10 else 0, 5), 10, "as an argument")
local made = {switch code case 404: 1 else 2, switch code case 200: 3 else 4}
assert_eq(#made .. ":" .. made[1] .. made[2], "2:14", "twice in one constructor")
-- Nesting, and the forms meeting each other.
assert_eq(switch 2 case 2: (switch 7 case 7: "in" else "no") else "out", "in",
          "an arm that is itself a switch expression")
assert_eq((|x| switch x case 1: "one" else "many")(7), "many",
          "a lambda whose body is a switch expression")
local narrowed = if code == 404 then 1 else 2
assert_eq(switch narrowed case 1: "yes" else "no", "yes",
          "a subject an 'if' expression computed")
-- Locals below it are untouched by the registers it uses.
local p, q, r = 1, 2, 3
assert_eq(switch r case 3: p + q else 0, 3, "reads locals")
assert_eq(p .. q .. r, "123", "and leaves them where they were")
assert_nocompile("return switch 1 case 1: 2", "'else' is required")
assert_nocompile("return switch 1 case 1: 2 default 3", "'default' is not the word")
-- 'switch' is a name in stock Lua and stays one. The three call shapes
-- test_interop pins stay calls, and an expression ending in the name
-- does not swallow the statement on the next line.
local switch = 5
local alias = switch
switch = switch + 1
assert_eq(alias .. "," .. switch, "5,6", "'switch' is still an ordinary name")
do
    local switch = |x| type(x)
    assert_eq(switch (21), "number", "'switch (x)' is still a call")
    assert_eq(switch {1, 2}, "table", "and so is 'switch {...}'")
    assert_eq(switch "s", "string", "and so is 'switch \"s\"'")
end

print("-- 7. The forms together")
local rows = {{name = "a", n = 1}, {name = "b", n = 2}, {name = "c", n = 3}}
local names = {}
for _, row in ipairs(rows[2:]) do
    local {name, n} = row
    names[#names + 1] = name .. (if n % 2 == 0 then "!" else "?")
end
assert_eq(table.concat(names, ","), "b!,c?",
          "a slice, a pattern and an 'if' expression in one loop")
assert_eq(table.concat((|t| t[1:2])({1, 2, 3}), ","), "1,2",
          "a lambda whose body is a slice")
local marks = {}
local mi = 0
for {name, n} in function() mi = mi + 1 return rows[mi] end do
    marks[#marks + 1] = name .. (switch n case 1: "!" case 2: "?" else ".")
end
assert_eq(table.concat(marks, ","), "a!,b?,c.",
          "a loop pattern and a switch expression together")
-- A pattern's names are the loop's, one set per iteration, so a closure
-- made in the body keeps the values that iteration saw.
local kept = {}
local ki = 0
for {name} in function() ki = ki + 1 return rows[ki] end do
    kept[#kept + 1] = |_| name
end
assert_eq(kept[1]() .. kept[2]() .. kept[3](), "abc",
          "a closure over a pattern name is fresh each iteration")

print("\n=== All Tier B Syntax Tests Passed ===")
