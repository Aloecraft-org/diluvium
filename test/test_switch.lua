-- test_switch.lua
-- Verifies the switch statement: semantics, and that making 'switch' a
-- contextual keyword did not take the name away from ordinary code.

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
    if load(src) == nil then
        print(string.format("[PASS] %s", name))
    else
        print(string.format("[FAIL] %s (compiled, expected a syntax error)", name))
        os.exit(1)
    end
end

print("=== Starting Switch Tests ===\n")

print("-- 1. Basic dispatch")
local function classify(x)
    switch x do
        case 1 then return "one"
        case 2 then return "two"
        default return "other"
    end
end
assert_eq(classify(1), "one", "first case")
assert_eq(classify(2), "two", "later case")
assert_eq(classify(9), "other", "default")

print("-- 2. Several values in one case")
local function group(x)
    switch x do
        case 1, 2, 3 then return "small"
        case 10, 20 then return "round"
        default return "other"
    end
end
assert_eq(group(1), "small", "first of a value list")
assert_eq(group(3), "small", "last of a value list")
assert_eq(group(20), "round", "second value list")
assert_eq(group(7), "other", "no list matched")

print("-- 3. No default")
local hit = "untouched"
switch 99 do
    case 1 then hit = "wrong"
end
assert_eq(hit, "untouched", "nothing runs when no case matches")

print("-- 4. The subject is evaluated exactly once")
local calls = 0
local function subject() calls = calls + 1 return 3 end
switch subject() do
    case 1 then error("wrong branch")
    case 2 then error("wrong branch")
    case 3 then hit = "third"
end
assert_eq(calls, 1, "subject evaluated once across several comparisons")
assert_eq(hit, "third", "...and matched the right case")

-- The value is frozen at entry, so a case expression that mutates the
-- subject variable cannot change what later cases compare against.
local n = 1
local function bump() n = 2 return "never" end
local outcome = "none"
switch n do
    case bump() then outcome = "bogus"
    case 2 then outcome = "saw the mutated value"
    default outcome = "still one"
end
assert_eq(outcome, "still one", "subject frozen against later mutation")

print("-- 5. No fallthrough")
local runs = {}
switch 1 do
    case 1 then runs[#runs + 1] = "a"
    case 1 then runs[#runs + 1] = "b"
    default runs[#runs + 1] = "c"
end
assert_eq(#runs, 1, "only one block runs")
assert_eq(runs[1], "a", "the first matching block is the one that runs")

print("-- 6. Value kinds and comparison semantics")
local function kind(v)
    switch v do
        case "s" then return "string"
        case 1.5 then return "float"
        case true then return "true"
        case false then return "false"
        case nil then return "nil"
        default return "other"
    end
end
assert_eq(kind("s"), "string", "string subject")
assert_eq(kind(1.5), "float", "float subject")
assert_eq(kind(true), "true", "boolean true")
assert_eq(kind(false), "false", "boolean false")
assert_eq(kind(nil), "nil", "nil subject matches a nil case")
assert_eq(kind({}), "other", "table falls to default")
-- Integers and floats compare equal in Lua, and switch inherits that.
assert_eq(kind(3 / 2), "float", "1.5 reached by arithmetic")

do  -- '==' semantics means __eq participates, as in an if-chain
    local mt = {__eq = function() return true end}
    local a, b = setmetatable({}, mt), setmetatable({}, mt)
    local got = "no"
    switch a do
        case b then got = "yes"
    end
    assert_eq(got, "yes", "__eq is honoured")
end

print("-- 7. Arbitrary expressions as case values")
local four = 4
switch four do
    case 2 + 2 then hit = "computed"
end
assert_eq(hit, "computed", "computed case value")
local t = {k = 5}
switch 5 do
    case t.k then hit = "indexed"
end
assert_eq(hit, "indexed", "case value from a table field")

print("-- 8. Nesting and control flow")
switch 1 do
    case 1 then
        switch 2 do
            case 2 then hit = "inner"
        end
end
assert_eq(hit, "inner", "switch nested in a case block")

local seen = {}
for i = 1, 5 do
    switch i do
        case 3 then break            -- breaks the enclosing loop
        default seen[#seen + 1] = i
    end
end
assert_eq(table.concat(seen, ","), "1,2", "'break' escapes the enclosing loop")

local function early(x)
    switch x do
        case 1 then return "returned"
    end
    return "fell through"
end
assert_eq(early(1), "returned", "'return' inside a case block")
assert_eq(early(2), "fell through", "control continues after a switch")

do  -- goto out of a case block
    local reached = false
    switch 1 do
        case 1 then goto out
    end
    reached = true
    ::out::
    assert_eq(reached, false, "'goto' escapes a case block")
end

print("-- 9. Scoping")
local shadow = "outer"
switch 1 do
    case 1 then
        local shadow = "inner"
        assert_eq(shadow, "inner", "a case block is its own scope")
end
assert_eq(shadow, "outer", "...and does not leak")

do  -- upvalues captured in a case block still close correctly
    local fns = {}
    for i = 1, 2 do
        switch i do
            case 1, 2 then local v = i * 10; fns[i] = function() return v end
        end
    end
    assert_eq(fns[1]() + fns[2](), 30, "closures over case-block locals")
end

print("-- 10. Empty forms")
switch 1 do end
switch 1 do default end
print("[PASS] empty switch and empty default bodies compile and run")

print("-- 11. 'switch' is still an ordinary name")
-- Every one of these is valid stock Lua and has to keep its meaning.
local switch = function(a) return "call:" .. type(a) end
assert_eq(switch(1), "call:number", "call with parentheses")
assert_eq(switch "s", "call:string", "call with a string argument")
assert_eq(switch {}, "call:table", "call with a table argument")

switch = 5
assert_eq(switch, 5, "assignment to a variable named 'switch'")
local tbl = {switch = 7}
assert_eq(tbl.switch, 7, "field named 'switch'")
assert_eq(tbl["switch"], 7, "string key 'switch'")
tbl.switch = 8
assert_eq(tbl.switch, 8, "assignment to a field named 'switch'")

local function takes(switch) return switch * 2 end
assert_eq(takes(21), 42, "parameter named 'switch'")

local acc = 0
for switch = 1, 3 do acc = acc + switch end
assert_eq(acc, 6, "loop variable named 'switch'")

local obj = {}
function obj:switch(v) return "method:" .. v end
assert_eq(obj:switch("ok"), "method:ok", "method named 'switch'")

do  -- the ambiguous shape: a call statement followed by a do-block
    local log = {}
    local switch = function(a) log[#log + 1] = a end
    switch (42) do log[#log + 1] = "block" end
    assert_eq(log[1], 42, "'switch (x) do ... end' is still a call")
    assert_eq(log[2], "block", "...followed by an ordinary do-block")
end

do  -- multiple assignment beginning with 'switch'
    local switch, other = 1, 2
    assert_eq(switch + other, 3, "multiple assignment starting with 'switch'")
end

print("-- 12. Malformed switches are syntax errors")
assert_nocompile([[switch 1 do case 1 then]], "unclosed switch")
assert_nocompile([[switch 1 do case 1 print(1) end]], "case without 'then'")
assert_nocompile([[switch 1 case 1 then end]], "switch without 'do'")
assert_nocompile([[switch do end]], "switch without a subject")
assert_nocompile([[switch 1 do print(1) end]], "a statement outside any case")
assert_nocompile([[switch 1 do default end case 1 then end]],
                 "'default' before 'case'")

print("\n=== All Tests Passed! ===")
