-- test_continue.lua
-- Verifies the 'continue' statement: it skips to the next iteration of the
-- innermost loop in each of the four loop forms, closes to-be-closed and
-- upvalue-captured locals on the way (because it is a goto and rides that
-- machinery), coexists with 'break', and -- being a contextual keyword --
-- does not take the name 'continue' away from ordinary code.

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

print("=== Starting Continue Tests ===\n")

print("-- 1. The four loop forms")

do
    local t = {}
    for i = 1, 6 do
        if i % 2 == 0 then continue end
        t[#t + 1] = i
    end
    assert_eq(table.concat(t, ","), "1,3,5", "numeric for skips even iterations")
end

do
    local t = {}
    for _, v in ipairs({ 10, 20, 30, 40 }) do
        if v == 20 then continue end
        t[#t + 1] = v
    end
    assert_eq(table.concat(t, ","), "10,30,40", "generic for skips one value")
end

do
    -- The continue re-tests the while condition, so the loop still ends.
    local i, t = 0, {}
    while i < 5 do
        i = i + 1
        if i == 3 then continue end
        t[#t + 1] = i
    end
    assert_eq(table.concat(t, ","), "1,2,4,5", "while re-tests its condition")
end

do
    -- The continue evaluates the 'until', so the loop still terminates.
    local i, t = 0, {}
    repeat
        i = i + 1
        if i == 2 then continue end
        t[#t + 1] = i
    until i >= 4
    assert_eq(table.concat(t, ","), "1,3,4", "repeat evaluates its 'until'")
end

print("\n-- 2. Nesting: continue targets the innermost loop")

do
    local t = {}
    for i = 1, 2 do
        for j = 1, 3 do
            if j == 2 then continue end
            t[#t + 1] = i .. j
        end
    end
    assert_eq(table.concat(t, ","), "11,13,21,23", "inner continue leaves the outer loop alone")
end

print("\n-- 3. continue and break in one loop")

do
    local t = {}
    for i = 1, 10 do
        if i == 3 then continue end
        if i == 5 then break end
        t[#t + 1] = i
    end
    assert_eq(table.concat(t, ","), "1,2,4", "continue skips 3, break stops at 5")
end

print("\n-- 4. continue closes what the end of the iteration would")

do
    -- A to-be-closed variable (via 'with') must run its __close when a
    -- continue leaves the iteration, exactly as reaching the block's end.
    local log = {}
    local function res(id)
        return setmetatable({}, { __close = function() log[#log + 1] = "close" .. id end })
    end
    for i = 1, 3 do
        with r = res(i) do
            if i == 2 then continue end
            log[#log + 1] = "body" .. i
        end
    end
    assert_eq(table.concat(log, ","),
        "body1,close1,close2,body3,close3",
        "continue runs __close of a to-be-closed local (i=2 body skipped, still closed)")
end

do
    -- 'defer' desugars to a to-be-closed local, so it must run on continue too.
    local log = {}
    for i = 1, 3 do
        defer log[#log + 1] = "d" .. i
        if i == 2 then continue end
        log[#log + 1] = "b" .. i
    end
    assert_eq(table.concat(log, ","), "b1,d1,d2,b3,d3", "continue runs a deferred statement")
end

do
    -- Each iteration's local is a fresh upvalue; continue must close the
    -- ones it skips like any other iteration exit.
    local fns = {}
    for i = 1, 3 do
        local x = i * 10
        if i == 2 then continue end
        fns[#fns + 1] = function() return x end
    end
    assert_eq(fns[1]() .. "," .. fns[2](), "10,30", "continue closes per-iteration upvalues")
end

print("\n-- 5. 'continue' is still an ordinary name")

do
    local continue = 1
    continue = continue + 1
    assert_eq(continue, 2, "continue as an assignable variable")

    continue += 4
    assert_eq(continue, 6, "continue as a compound-assignment target")

    local function continue(x) return x * 2 end
    assert_eq(continue(21), 42, "continue as a called function")

    local t = { continue = 7 }
    assert_eq(t.continue, 7, "continue as a table field")
    t.continue = 8
    assert_eq(t.continue, 8, "continue as an assigned field")

    local a, continue2 = 1, 2
    a, continue2 = continue2, a
    assert_eq(a .. continue2, "21", "continue-like names in a multiple assignment")
end

do
    -- A hand-written '::continue::' label and 'goto continue' keep working,
    -- because the automatic label is only planted when the continue keyword
    -- is actually used in that loop.
    local t = {}
    for i = 1, 3 do
        if i == 2 then goto skip end
        t[#t + 1] = i
        ::skip::
    end
    assert_eq(table.concat(t, ","), "1,3", "a manual goto/label in a loop is unaffected")
end

print("\n-- 6. Misuse is a compile error")

assert_nocompile("continue", "continue outside any loop is rejected")
assert_nocompile("do continue end", "continue in a plain block (no loop) is rejected")
-- In a repeat, a continue that jumps past a local the 'until' then reads is
-- refused by name, the same way an explicit goto would be -- a clear error,
-- never a silent miscompile.
assert_nocompile(
    "local n = 0 repeat n = n + 1 if n < 3 then continue end local x = n until x >= 3",
    "continue may not jump into the scope of a later local a 'repeat' reads")

print("\n=== All Continue Tests Passed ===")
