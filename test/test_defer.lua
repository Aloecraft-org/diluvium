-- test_defer.lua
-- Verifies the defer statement: ordering, every way out of a block,
-- and that making 'defer' a contextual keyword did not take the name
-- away from ordinary code.

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

local log
local function say(s) log[#log + 1] = s end
local function trace() return table.concat(log, ",") end

print("=== Starting Defer Tests ===\n")

print("-- 1. Forms and ordering")
log = {}
do
    defer say("a")
    defer say("b")
    say("body")
end
assert_eq(trace(), "body,b,a", "runs after the block, in reverse order")

log = {}
do
    defer do say("b1") say("b2") end
    say("body")
end
assert_eq(trace(), "body,b1,b2", "block form via 'defer do ... end'")

log = {}
do
    defer say("only")
end
assert_eq(trace(), "only", "a block whose only statement is a defer")

print("-- 2. Every way out of a block")
log = {}
local ok, err = pcall(function()
    defer say("on-error")
    error("boom")
end)
assert_eq(trace(), "on-error", "runs when an error unwinds the block")
assert_eq(ok, false, "...and the error still propagates")
assert_eq(err:gsub(".*: ", ""), "boom", "...unchanged")

log = {}
local function early()
    defer say("on-return")
    return "value"
end
assert_eq(early(), "value", "the return value is untouched")
assert_eq(trace(), "on-return", "runs on return")

log = {}
do
    defer say("on-goto")
    goto out
end
::out::
assert_eq(trace(), "on-goto", "runs when goto leaves the block")

log = {}
for i = 1, 3 do
    defer say("i" .. i)
    if i == 2 then break end
end
assert_eq(trace(), "i1,i2", "runs once per iteration, and on break")

log = {}
for i = 1, 2 do
    defer say("body" .. i)
    goto continue
    ::continue::
end
assert_eq(trace(), "body1,body2", "runs once per iteration with a continue label")

print("-- 3. Nesting")
log = {}
do
    defer say("outer")
    do defer say("inner") end
    say("between")
end
assert_eq(trace(), "inner,between,outer", "inner block closes first")

log = {}
do
    defer do defer say("nested-inner") say("nested-outer") end
end
assert_eq(trace(), "nested-outer,nested-inner", "a defer inside a deferred block")

print("-- 4. Captures and scoping")
log = {}
do
    local captured = "first"
    defer say("saw:" .. captured)
    captured = "second"
end
assert_eq(trace(), "saw:second", "the body reads the variable at close time, not at defer time")

log = {}
for i = 1, 2 do
    local v = i * 10
    defer say("v" .. v)
end
assert_eq(trace(), "v10,v20", "each iteration captures its own local")

do
    local fns = {}
    log = {}
    for i = 1, 2 do
        defer do fns[i] = function() return i end end
    end
    assert_eq(fns[1]() + fns[2](), 3, "closures created inside a deferred block")
end

print("-- 5. Errors raised by the deferred code")
log = {}
local ok2, err2 = pcall(function()
    defer error("from-defer")
    say("body")
end)
assert_eq(ok2, false, "an error in the deferred code propagates")
assert_eq(tostring(err2):gsub(".*: ", ""), "from-defer", "...with its own message")

-- Lua's to-be-closed rule: an error raised while unwinding replaces the
-- one already in flight. Inherited, not chosen; asserted so it stays known.
local ok3, err3 = pcall(function()
    defer error("second")
    error("first")
end)
assert_eq(ok3, false, "both errors, outer one reported")
assert_eq(tostring(err3):gsub(".*: ", ""), "second",
          "an error while unwinding replaces the in-flight error")

print("-- 6. Coroutines")
log = {}
local co = coroutine.create(function()
    defer say("closed")
    coroutine.yield()
    say("never reached")
end)
coroutine.resume(co)
assert_eq(trace(), "", "nothing runs while the coroutine is suspended")
coroutine.close(co)
assert_eq(trace(), "closed", "coroutine.close runs pending defers")

log = {}
local co2 = coroutine.create(function()
    defer say("after-yield")
    coroutine.yield("mid")
    say("resumed")
end)
coroutine.resume(co2)
coroutine.resume(co2)
assert_eq(trace(), "resumed,after-yield", "a defer survives a yield and runs at the end")

print("-- 7. Interaction with other Diluvium constructs")
log = {}
switch 2 do
    case 2 then
        defer say("in-case")
        say("case-body")
end
assert_eq(trace(), "case-body,in-case", "defer inside a switch case block")

log = {}
do
    local n = 0
    defer say("n=" .. n)
    n += 5
end
assert_eq(trace(), "n=5", "defer alongside compound assignment")

log = {}
do
    local who = "world"
    defer say($"bye {who}")
end
assert_eq(trace(), "bye world", "f-string inside a deferred statement")

print("-- 8. Alongside user-declared to-be-closed variables")
log = {}
do
    local mt = {__close = function() say("user-close") end}
    local _u <close> = setmetatable({}, mt)
    defer say("defer")
end
assert_eq(trace(), "defer,user-close", "defer and <close> interleave in one reverse order")

print("-- 9. 'defer' is still an ordinary name")
local defer = function(a) return "call:" .. type(a) end
assert_eq(defer(1), "call:number", "call with parentheses")
assert_eq(defer "s", "call:string", "call with a string argument")
assert_eq(defer {}, "call:table", "call with a table argument")

defer = 5
assert_eq(defer, 5, "assignment to a variable named 'defer'")
local tbl = {defer = 7}
assert_eq(tbl.defer, 7, "field named 'defer'")
assert_eq(tbl["defer"], 7, "string key 'defer'")
tbl.defer = 8
assert_eq(tbl.defer, 8, "assignment to a field named 'defer'")

local function takes(defer) return defer * 2 end
assert_eq(takes(21), 42, "parameter named 'defer'")

local acc = 0
for defer = 1, 3 do acc = acc + defer end
assert_eq(acc, 6, "loop variable named 'defer'")

local obj = {}
function obj:defer(v) return "method:" .. v end
assert_eq(obj:defer("ok"), "method:ok", "method named 'defer'")

do  -- the ambiguous shape: a call statement whose argument is parenthesised
    local seen = {}
    local defer = function(a) seen[#seen + 1] = a end
    defer (42)
    assert_eq(seen[1], 42, "'defer (x)' is still a call")
end

print("-- 10. Malformed defers are syntax errors")
assert_nocompile([[defer]], "'defer' with nothing after it")
assert_nocompile([[defer do print(1)]], "unclosed deferred block")

-- 'defer' is only a keyword at the start of a statement. In expression
-- position it stays an ordinary name, so this is a global read followed
-- by a separate call statement -- exactly what stock Lua makes of it.
do
    local chunk = load([[local x = defer  print(1)  return x]])
    assert_eq(type(chunk), "function", "'defer' in expression position is a name")
    assert_eq(chunk(), nil, "...reading an unset global, as stock Lua does")
end

print("\n=== All Tests Passed! ===")
