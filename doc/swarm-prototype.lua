-- Deterministic actor scheduler -- a runnable sketch of doc/swarm.md.
--
-- Run it:  ./dist/diluvium_debug doc/swarm-prototype.lua
--
-- This is a MODEL, not a foundation. It implements the in-process tier
-- only: one atomic counter for sequencing, actors as coroutines in a
-- single state. It exists to make the yield/resume model concrete and to
-- prove determinism by replay rather than by assertion. What it leaves out
-- is listed at the end of doc/swarm.md.
--
-- The one idea worth taking away: recv() and a hostcall are the SAME
-- mechanism. An actor yields a request; the scheduler answers it and
-- resumes. Whether the answer was ready immediately (a pure hostcall) or
-- arrives much later (an empty mailbox, or real host I/O) is invisible to
-- the actor. That indistinguishability is the whole async story, and it is
-- why messaging and the hostcall ABI are one design and not two.

local swarm = {}

do
  -- ---- scheduler state -------------------------------------------------
  local seq                -- the logical clock: a plain monotonic counter
  local actors             -- pid -> { co, mailbox, dead, name }
  local next_pid
  local topics             -- topic name -> ordered list of subscriber pids
  local host               -- registered host functions (the kernel surface)
  local trace              -- the deterministic delivery log, our proof object

  function swarm.reset()
    seq, next_pid = 0, 0
    actors, topics, host, trace = {}, {}, {}, {}
  end

  local function nextseq()
    seq = seq + 1
    return seq
  end

  -- Run one actor forward from a delivered value until it blocks on recv
  -- or finishes. A single "turn" can perform any number of hostcalls --
  -- each is answered and the actor resumed -- before it blocks again.
  local function step(pid, value)
    local a = actors[pid]
    while true do
      local ok, req = coroutine.resume(a.co, value)
      if not ok then
        error("actor '" .. a.name .. "' crashed: " .. tostring(req))
      end
      if coroutine.status(a.co) == "dead" then
        a.dead = true
        return
      end
      -- 'req' is what the actor wants next.
      if req.op == "recv" then
        return                 -- blocked; the scheduler will deliver later
      elseif req.op == "call" then
        local fn = host[req.name]
        if not fn then error("no such hostcall: " .. tostring(req.name)) end
        -- Performed immediately here. A real async host would instead
        -- record the request, leave the actor blocked exactly as recv
        -- does, and resume it when the completion arrived -- same path.
        value = fn(table.unpack(req.args, 1, req.args.n))
      else
        error("unknown request: " .. tostring(req.op))
      end
    end
  end

  -- The deterministic pick: among actors that are blocked on recv and have
  -- mail, the one whose oldest message has the globally lowest sequence
  -- number. Sequence numbers are unique, so this is a total order -- the
  -- result does not depend on pairs() iteration order at all.
  local function pick()
    local best_pid, best_seq
    for pid, a in pairs(actors) do
      if not a.dead and #a.mailbox > 0
         and coroutine.status(a.co) == "suspended" then
        local head_seq = a.mailbox[1].seq
        if best_seq == nil or head_seq < best_seq then
          best_pid, best_seq = pid, head_seq
        end
      end
    end
    return best_pid
  end

  -- ---- the actor-facing API -------------------------------------------

  -- Blocking operations. These yield a request to the scheduler.
  function swarm.receive()
    return coroutine.yield({ op = "recv" })
  end

  function swarm.call(name, ...)
    return coroutine.yield({ op = "call", name = name, args = table.pack(...) })
  end

  -- Non-blocking operations. These just mutate scheduler state and return;
  -- nothing about send or spawn needs to suspend the caller.
  function swarm.spawn(name, body)
    local pid = next_pid
    next_pid = next_pid + 1
    actors[pid] = { co = coroutine.create(body), mailbox = {},
                    dead = false, name = name }
    -- Birth: run the body up to its first block, so it can do setup and
    -- send initial messages at a well-defined point in the caller's turn.
    step(pid, nil)
    return pid
  end

  function swarm.send(pid, msg)
    local a = actors[pid]
    if not a or a.dead then return false end   -- to a dead actor: dropped
    a.mailbox[#a.mailbox + 1] = { seq = nextseq(), msg = msg }
    return true
  end

  function swarm.register(name, fn)   -- host installs a kernel function
    host[name] = fn
  end

  -- pub/sub, in four lines, on top of send: a topic fans a message out by
  -- sending one sequence-numbered copy to each subscriber. Fan-out built
  -- from fan-in, exactly as doc/swarm.md argues.
  function swarm.subscribe(topic, pid)
    topics[topic] = topics[topic] or {}
    topics[topic][#topics[topic] + 1] = pid
  end

  function swarm.publish(topic, msg)
    for _, pid in ipairs(topics[topic] or {}) do
      swarm.send(pid, msg)
    end
  end

  -- ---- the run loop ----------------------------------------------------
  -- Drain every deliverable message, in global sequence order, until the
  -- swarm is quiescent (no actor is both blocked and holding mail).
  function swarm.run()
    while true do
      local pid = pick()
      if not pid then return end
      local a = actors[pid]
      local m = table.remove(a.mailbox, 1)   -- the head is the lowest seq
      trace[#trace + 1] = { seq = m.seq, name = a.name }
      step(pid, m.msg)
    end
  end

  function swarm.trace() return trace end
end


-- =====================================================================
-- DEMO: a map-reduce swarm.
--
--   * a coordinator collects partial sums,
--   * four workers each sum a range of ten integers and reply,
--   * workers publish progress to a "progress" topic,
--   * a logger subscribes to that topic and counts,
--   * each worker folds a host-computed checksum into its reply, to show a
--     hostcall in the compute path riding the same scheduler.
--
-- The interesting part is the interleaving: the logger's progress
-- deliveries and the coordinator's result deliveries land intermixed, in
-- strict sequence order, and that order is reproduced exactly on replay.
-- =====================================================================

local function demo()
  swarm.reset()

  -- A host function: pure and deterministic, so it does not threaten the
  -- replay. A nondeterministic host call (a clock, a random source) would
  -- have to be sequence-stamped to stay replayable -- see doc/swarm.md.
  swarm.register("checksum", function(n)
    return (n * 2654435761) % 4294967296
  end)

  local report = { total = 0, replies = 0, logged = 0 }

  local logger = swarm.spawn("logger", function()
    while true do
      swarm.receive()
      report.logged = report.logged + 1
    end
  end)
  swarm.subscribe("progress", logger)

  local coord = swarm.spawn("coord", function()
    while true do
      local m = swarm.receive()          -- { value = n, check = c }
      report.total = report.total + m.value
      report.replies = report.replies + 1
    end
  end)

  for w = 1, 4 do
    local lo, hi = (w - 1) * 10 + 1, w * 10
    local worker = swarm.spawn("worker" .. w, function()
      local m = swarm.receive()          -- { lo, hi, reply_to }
      local s = 0
      for i = m.lo, m.hi do
        s = s + i
        if i % 5 == 0 then
          swarm.publish("progress", { worker = w, at = i })
        end
      end
      local check = swarm.call("checksum", s)   -- a hostcall, mid-compute
      swarm.send(m.reply_to, { value = s, check = check })
    end)
    swarm.send(worker, { lo = lo, hi = hi, reply_to = coord })
  end

  swarm.run()

  -- serialise the delivery trace: this string is what must be identical
  -- across runs for the swarm to be deterministic.
  local parts = {}
  for _, e in ipairs(swarm.trace()) do
    parts[#parts + 1] = e.seq .. ":" .. e.name
  end
  report.trace = table.concat(parts, "  ")
  return report
end


local a = demo()
local b = demo()

print("sum of 1..40      : " .. a.total .. "  (expected 820)")
print("worker replies    : " .. a.replies)
print("progress logged   : " .. a.logged)
print("deliveries         : " .. (#swarm.trace()))
print()
print("delivery order (seq:actor):")
print("  " .. a.trace)
print()

assert(a.total == 820, "wrong sum")
assert(a.replies == 4, "lost a reply")
assert(a.trace == b.trace, "NON-DETERMINISTIC: the two runs diverged")
print("replay is byte-identical: the swarm is deterministic.")
