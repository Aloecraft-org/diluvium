-- test_queue.lua
-- doc/Messaging.md 14 asks for one test per row of the status table in 6.4,
-- "the rows that will regress". Those are grouped together below and named
-- after the status they cover, so a failure says which row broke.
--
-- The rest covers the model rather than the API surface: that a value crosses
-- as a copy and not a reference, that a bounded queue stays bounded under a
-- ring wrap, and that a handle is runtime identity only.

local pass, fail = 0, 0

local function ok(cond, what)
    if cond then pass = pass + 1
    else fail = fail + 1; print(string.format("[FAIL] %s", what)) end
end

local function eq(actual, expected, what)
    if actual == expected then pass = pass + 1
    else
        fail = fail + 1
        print(string.format("[FAIL] %s\n  expected %s\n  actual   %s",
              what, tostring(expected), tostring(actual)))
    end
end

local n = 0
local function fresh(opts)
    n = n + 1
    return queue.declare("t/" .. n, opts)
end

print("=== queue ===")

print("-- reserved names are declared for us (6.6)")
for _, name in ipairs{"inbox", "outbox"} do
    local id = queue.lookup(name)
    ok(id ~= nil, name .. " exists without being declared")
    ok(queue.info(id).exported, name .. " is exported")
end
eq(queue.lookup("nope"), nil, "lookup of an undeclared name is nil")

print("-- declare and its options")
do
    local id = queue.declare("t/opts", {capacity = 4, on_full = "drop_oldest",
                                        exported = true, direction = "guest_read"})
    local i = queue.info(id)
    eq(i.capacity, 4, "capacity is recorded")
    eq(i.on_full, "drop_oldest", "on_full is recorded")
    eq(i.exported, true, "exported is recorded")
    -- 6.3: direction is recorded but not enforced in this milestone. Asserting
    -- that it round-trips is what stops the field being quietly dropped before
    -- enforcement lands, which would be a wire-format change later.
    eq(i.direction, "guest_read", "direction is recorded (not yet enforced)")
    eq(queue.capacity(id), 4, "capacity() agrees")
    eq(queue.len(id), 0, "a new queue is empty")
    eq(queue.state(id), "enabled", "a new queue is enabled")

    eq(queue.info(queue.lookup("inbox")).capacity, 64, "the default capacity is 64")
    eq(queue.info(queue.lookup("inbox")).on_full, "reject",
       "the default on_full is reject")
    eq(queue.info(queue.lookup("inbox")).direction, "both",
       "the default direction is both")
    eq(queue.info(queue.lookup("inbox")).exported, true,
       "but inbox is exported by default")

    ok(not pcall(queue.declare, "t/opts"), "a duplicate name is refused")
    ok(not pcall(queue.declare, "t/bad", {capacity = 0}),
       "capacity 0 is refused: queues are bounded, not empty")
    ok(not pcall(queue.declare, "t/bad", {capacity = -1}),
       "a negative capacity is refused")
    ok(not pcall(queue.declare, "t/bad", {on_full = "shrug"}),
       "an unknown on_full is refused")
    ok(not pcall(queue.declare, "t/bad", {direction = "sideways"}),
       "an unknown direction is refused")
    -- 'block' is in the grammar but needs the wait-set protocol. Refusing it
    -- is better than treating it as 'reject', which would hand a program the
    -- opposite of the backpressure it asked for and look like it worked.
    local okk, err = pcall(queue.declare, "t/bad", {on_full = "block"})
    ok(not okk, "on_full 'block' is refused in this milestone")
    ok(tostring(err):find("wait", 1, true) ~= nil,
       "and says what it is waiting on: " .. tostring(err))
end

print("-- the status table in 6.4, one case per row")
do
    -- "ok": accepted
    local q = fresh{capacity = 2}
    local okk, st = queue.push(q, "a")
    eq(okk, true, [["ok": push returns true]])
    eq(st, "ok", [["ok"]])

    -- "full": at capacity, not accepted
    queue.push(q, "b")
    okk, st = queue.push(q, "c")
    eq(okk, false, [["full": push returns false]])
    eq(st, "full", [["full"]])
    eq(queue.len(q), 2, "and nothing was added")

    -- "dropped_oldest": accepted, oldest evicted
    local d = fresh{capacity = 2, on_full = "drop_oldest"}
    queue.push(d, "a"); queue.push(d, "b")
    okk, st = queue.push(d, "c")
    eq(okk, true, [["dropped_oldest": push returns true]])
    eq(st, "dropped_oldest", [["dropped_oldest"]])
    eq(queue.len(d), 2, "the queue is still at capacity")
    eq(queue.pop(d), "b", "the oldest went, so 'b' is now first")
    eq(queue.pop(d), "c", "then the one that displaced it")

    -- drop_newest: 6.4 has no separate status for it, because the newest
    -- message is the one being pushed -- dropping it and rejecting it are the
    -- same event, and it reports "full".
    local dn = fresh{capacity = 1, on_full = "drop_newest"}
    queue.push(dn, "keep")
    okk, st = queue.push(dn, "toss")
    eq(okk, false, "drop_newest reports not-accepted")
    eq(st, "full", "with 'full', since the newest is the one refused")
    eq(queue.pop(dn), "keep", "and the queue kept what it had")

    -- "disabled": queue disabled, not accepted
    local e = fresh{}
    queue.disable(e)
    eq(queue.state(e), "disabled", "disable() shows in state()")
    okk, st = queue.push(e, "x")
    eq(okk, false, [["disabled": push returns false]])
    eq(st, "disabled", [["disabled"]])
    queue.enable(e)
    eq(queue.state(e), "enabled", "enable() restores it")
    okk, st = queue.push(e, "x")
    eq(st, "ok", "and pushes are accepted again")

    -- "empty": nothing to pop
    local p = fresh{}
    local v
    v, st = queue.pop(p)
    eq(v, nil, [["empty": pop returns nil]])
    eq(st, "empty", [["empty"]])

    -- A disabled queue still gives up what it already holds: 6.1 says disable
    -- is so a program going down rejects new messages cleanly, not so it
    -- abandons the ones it accepted.
    local dd = fresh{}
    queue.push(dd, "held")
    queue.disable(dd)
    eq(queue.pop(dd), "held", "a disabled queue can still be drained")
end

print("-- a full, disabled or dropping destination is not an error (6.4)")
do
    -- The delivery-semantics test 14 asks for: none of these may raise.
    local q = fresh{capacity = 1}
    queue.push(q, 1)
    ok(pcall(queue.push, q, 2), "pushing to a full queue does not raise")
    local e = fresh{}
    queue.disable(e)
    ok(pcall(queue.push, e, 1), "pushing to a disabled queue does not raise")
    ok(pcall(queue.pop, fresh{}), "popping an empty queue does not raise")
end

print("-- values cross as copies, not references (6.5)")
do
    local q = fresh{}
    local t = {a = 1, nested = {2, 3}}
    queue.push(q, t)
    t.a = 99                      -- mutate after pushing
    t.nested[1] = 99
    local got = queue.pop(q)
    eq(got.a, 1, "the sender cannot change a message after pushing it")
    eq(got.nested[1], 2, "including nested tables")
    ok(got ~= t, "and the value is a different table")
end

print("-- every msgpack-encodable value survives a queue")
do
    local q = fresh{capacity = 32}
    local cases = {1, -1, 0, 1.5, math.maxinteger, math.mininteger, true, false,
                   "", "text", "with\0zero", {}, {1, 2, 3}, {a = {b = "c"}}}
    for i = 1, #cases do queue.push(q, cases[i]) end
    for i = 1, #cases do
        local got = queue.pop(q)
        if type(cases[i]) == "table" then
            eq(type(got), "table", "case " .. i .. " is still a table")
        else
            eq(got, cases[i], "case " .. i .. " round-trips")
        end
    end
    -- nil is encodable, so it can be pushed; the "empty" status is what
    -- distinguishes an absent message from a nil one.
    queue.push(q, nil)
    local v, st = queue.pop(q)
    eq(v, nil, "a pushed nil comes back nil")
    eq(st, "ok", "with status ok, which is how it differs from empty")
end

print("-- an unencodable value is refused, and changes nothing")
do
    local q = fresh{}
    ok(not pcall(queue.push, q, print), "pushing a function raises")
    eq(queue.len(q), 0, "and the queue is untouched")
    local _, err = pcall(queue.push, q, {bad = coroutine.create(function() end)})
    ok(tostring(err):find("bad", 1, true) ~= nil,
       "the key path from 5.6 survives into a queue push: " .. tostring(err))
end

print("-- bounded means bounded, including across a ring wrap")
do
    local q = fresh{capacity = 3}
    -- Push and pop far more than capacity, so the ring wraps many times.
    for round = 1, 20 do
        for i = 1, 3 do
            local okk, st = queue.push(q, round * 100 + i)
            eq(st, "ok", "round " .. round .. " push " .. i)
        end
        eq(queue.len(q), 3, "at capacity after filling, round " .. round)
        eq(select(2, queue.push(q, "over")), "full",
           "refuses a fourth, round " .. round)
        for i = 1, 3 do
            eq(queue.pop(q), round * 100 + i,
               "FIFO order held, round " .. round .. " item " .. i)
        end
        eq(queue.len(q), 0, "drained, round " .. round)
    end
end

print("-- interleaved push and pop keeps FIFO order")
do
    local q = fresh{capacity = 4}
    queue.push(q, 1); queue.push(q, 2)
    eq(queue.pop(q), 1, "first out is first in")
    queue.push(q, 3)
    eq(queue.pop(q), 2, "then the second")
    queue.push(q, 4); queue.push(q, 5)
    eq(queue.pop(q), 3, "order survives interleaving")
    eq(queue.pop(q), 4, "and continues")
    eq(queue.pop(q), 5, "to the end")
    eq(select(2, queue.pop(q)), "empty", "then empty")
end

print("-- handles are runtime identity, not durable identity (6.2)")
do
    local a = fresh{}
    local b = fresh{}
    ok(a ~= b, "handles are distinct")
    ok(math.type(a) == "integer", "and integers, not strings")
    local id = queue.declare("t/gone")
    queue.destroy(id)
    eq(queue.lookup("t/gone"), nil, "a destroyed queue is no longer findable")
    -- A handle is never reused, so using a stale one is an error rather than a
    -- silent hit on whatever took its place. 6.2 warns that the latter is the
    -- obvious mistake and that it fails quietly.
    local okk, err = pcall(queue.push, id, 1)
    ok(not okk, "a stale handle raises rather than addressing another queue")
    ok(tostring(err):find("destroyed", 1, true) ~= nil,
       "and says the handle was destroyed: " .. tostring(err))
    ok(queue.declare("t/gone") ~= id, "the name can be re-declared, with a new handle")
    ok(not pcall(queue.push, 99999, 1), "an unissued handle raises too")
    ok(not pcall(queue.push, 0, 1), "handle 0 is not valid")
end

print("-- pop never yields (6.3)")
do
    -- The distinction that keeps every read from being a suspension point: an
    -- empty queue is a value, not a wait. Checked inside a coroutine, where a
    -- yield would be legal and therefore possible.
    local co = coroutine.create(function()
        local q = queue.declare("t/noyield")
        local v, st = queue.pop(q)
        return st
    end)
    local okk, st = coroutine.resume(co)
    ok(okk, "the coroutine ran")
    eq(st, "empty", "pop on an empty queue returned rather than parking")
    eq(coroutine.status(co), "dead", "and the coroutine is finished, not suspended")
end

print("-- the proc_10ms / proc_50ms / proc_500ms pattern, end to end")
do
    -- 13's acceptance criterion for this milestone. Without queue.wait it has
    -- to be polled, which is expected here and not a workaround: the point is
    -- that a chain of bounded queues carries values between stages at
    -- different rates without loss or unbounded growth.
    local fast = queue.declare("proc_10ms", {capacity = 8})
    -- Sized for what it receives: ten pushes over fifty ticks. It was 8 while
    -- this was being written, and the sum came out as sum(1..40) rather than
    -- sum(1..50) -- the queue correctly rejected the last two pushes and the
    -- test had simply under-sized it. The overflow case is pinned on purpose
    -- below rather than met by accident here.
    local mid = queue.declare("proc_50ms", {capacity = 16})
    local slow = queue.declare("proc_500ms", {capacity = 8, on_full = "drop_oldest"})

    local sums = {}
    for tick = 1, 50 do
        queue.push(fast, {tick = tick, v = tick})
        if tick % 5 == 0 then          -- the 50ms stage drains the 10ms one
            local total = 0
            while true do
                local m = queue.pop(fast)
                if m == nil then break end
                total = total + m.v
            end
            queue.push(mid, {tick = tick, total = total})
        end
        if tick % 50 == 0 then         -- the 500ms stage drains the 50ms one
            local grand = 0
            while true do
                local m = queue.pop(mid)
                if m == nil then break end
                grand = grand + m.total
            end
            queue.push(slow, {grand = grand})
        end
    end
    local out = queue.pop(slow)
    ok(out ~= nil, "a value reached the slowest stage")
    eq(out.grand, (50 * 51) // 2, "carrying the sum of every tick, with none lost")
    eq(queue.len(fast), 0, "the fast queue drained")
    eq(queue.len(mid), 0, "the mid queue drained")
    ok(queue.len(fast) <= queue.capacity(fast), "and stayed within capacity")
end

print("-- an under-sized stage loses messages, and says so")
do
    -- The other half of "bounded, always": a stage that cannot keep up does
    -- not grow, and the sender is told which messages did not make it. This is
    -- the shape the pattern above hit when its middle queue was too small.
    local small = queue.declare("proc/small", {capacity = 3})
    local accepted, refused = 0, 0
    for i = 1, 10 do
        local okk, st = queue.push(small, i)
        if okk then accepted = accepted + 1
        else
            refused = refused + 1
            eq(st, "full", "a refused push says why")
        end
    end
    eq(accepted, 3, "only capacity-many were accepted")
    eq(refused, 7, "and the sender was told about every one that was not")
    eq(queue.len(small), 3, "the queue did not grow past its bound")
end

print("-- argument checking")
do
    ok(not pcall(queue.declare), "declare needs a name")
    ok(not pcall(queue.declare, 5), "a numeric name is refused")
    ok(not pcall(queue.declare, "t/x", "opts"), "non-table opts are refused")
    ok(not pcall(queue.push, queue.lookup("inbox")), "push needs a value")
    ok(not pcall(queue.pop), "pop needs a handle")
    ok(not pcall(queue.len, "inbox"), "len takes a handle, not a name")
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
