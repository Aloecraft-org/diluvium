--[[
supervisor.lua -- the root of the swarm, and the only program that spawns.

There is no supervisor type in Diluvium, and that is the design rather than an
omission (doc/Messaging.md 9.1). This is an ordinary program. The only thing that
makes it a supervisor is that it holds the "lifecycle" capability, so the swarm
layer bothers to read its system/lifecycle queue at all -- a program without that
capability may declare the queue and write to it all it likes, and nothing will
ever read it.

WHY EVERY INSTANCE HANGS OFF THIS ONE

Handlers used to be children of the coordinator, which read well on a diagram and
was wrong for two reasons that both come from the same fact: when an instance
stops, the swarm layer kills its whole subtree.

  A coordinator crash took every session with it. Not the coordinator's memory of
  the sessions -- the sessions. Matchmaking is the most exposed program in this
  swarm (it touches every arrival, it parses every message), so the riskiest
  program was also the one holding everyone's life in its hands.

  And there could only ever be one of it. A coordinator that owns its handlers
  cannot be sharded, so one request that stalls it for three seconds blocks every
  arrival behind it. Head-of-line blocking in the one place a matchmaker cannot
  afford it.

Now handlers are children of *this*, and the coordinator holds no lifecycle
capability at all. It asks; this spawns. That costs a message round trip per
admission and buys crash isolation and the ability to run coordinators in
parallel, each caching whatever addressing it likes, none of them owning a
session.

The split is also cleaner than the round trip is expensive: this program owns
"which instances exist", and the coordinator owns "these two should be paired".
]]

local sys = queue.declare('system/lifecycle', {capacity = 32})
local ev  = queue.declare('system/events',    {capacity = 64})
local log = queue.declare('log',   {capacity = 64, exported = true})
-- How the host learns which instance became the coordinator. Ids are the swarm's
-- to assign, so a program that wants the host to route to a peer must say so.
local roles = queue.declare('roles', {capacity = 4, exported = true})
-- The host pushes the coordinator's source in here, as an ordinary message.
-- Code is data: there is nothing special about a string that happens to be a
-- program, and for a system meant to rewrite its own agents at runtime that is
-- the feature rather than a curiosity.
local codeq = queue.declare('code', {capacity = 2})
-- Requests arrive from the coordinator here; answers go back through outbox.
-- The host moves both, unread.
local inb = queue.lookup('inbox')
local out = queue.lookup('outbox')

local function say (s) queue.push(log, s) end

say('supervisor up; waiting for the coordinator program')
local _, COORD_SRC = queue.wait({codeq})
say(('got the coordinator program (%d bytes)'):format(#COORD_SRC))

--[[
The spawn request, in the shape 9.1 documents.

  caps -- strictly narrower than what this program holds. The root holds
  "queue:*"; the coordinator gets "queue:client/*", and no longer gets
  "lifecycle" at all, because it no longer spawns anything.

  budget -- nested, as 9.1 shows it. This was silently ignored until recently:
  the child ran unbudgeted and the spawn still reported success, which is the
  kind of defect that only surfaces when something runs away.
]]
local function spawn_coordinator ()
  queue.push(sys, {
    op    = 'spawn',
    code  = COORD_SRC,
    caps  = { 'queue:client/*' },
    budget = { instructions = 20000000, memory_kb = 2048 },
  })
end

spawn_coordinator()

local coord     = nil   -- the current coordinator's instance id
local restarts  = 0
local pending   = {}    -- spawns asked for, oldest first
local handlers  = {}    -- instance id -> the request that made it

-- Tell the coordinator something. It may not exist yet, in which case the news
-- is dropped -- which is correct: a coordinator that has not started has nothing
-- to reconcile, and one that restarts starts empty by design.
local function tell (m)
  if coord then queue.push(out, m) end
end

while true do
  local q, m = queue.wait({ev, inb})

  if q == inb then
    -- A request from the coordinator. It holds no lifecycle capability, so this
    -- is the only way a handler can come into existence.
    if m.op == 'need_handler' then
      --[[
      THE COST OF DELEGATING SPAWN, and the thing this example exists to find.

      While the coordinator spawned its own handlers, 9.3 was enforced by the
      runtime: it held 'queue:client/*', so it could not grant more, and no
      mistake in it could widen a grant. Now that it asks and this program
      spawns, the swarm layer checks the request against *this* program's caps --
      and this one holds the root's ceiling. So a coordinator can now ask for
      anything up to 'lifecycle, queue:*' and the runtime will allow it.

      Measured, not reasoned about: with this check absent, the 'greedy' client's
      handler was spawned holding 'lifecycle'.

      So the check has to live here, in a program, which is strictly weaker than
      having the runtime do it -- a bug in this function is a privilege
      escalation, where before it was impossible. That is the price of crash
      isolation and sharding, and it is worth paying only because it is a dozen
      lines in one place. The runtime still backstops anything outside root's own
      ceiling; what it can no longer see is the coordinator's narrower one.
      ]]
      local GRANTABLE = 'queue:client/'
      local bad = nil
      for _, c in ipairs(m.caps or {}) do
        if c:sub(1, #GRANTABLE) ~= GRANTABLE then bad = c end
      end
      if bad then
        say(('spawn for %s refused: %s is outside what a coordinator may ask for')
            :format(tostring(m.client), bad))
        tell({op = 'refused', client = m.client,
              detail = bad .. ' is not a coordinator\'s to grant'})
      else
        pending[#pending + 1] = m
        queue.push(sys, {
          op   = 'spawn',
          code = m.code,
          caps = m.caps,
          budget = { instructions = 500000, memory_kb = 256 },
        })
      end

    elseif m.op == 'release' then
      -- The coordinator matched a pair and wants both handlers gone. It cannot
      -- kill them itself: only an ancestor may kill, and it is not one any more.
      for _, id in ipairs(m.ids or {}) do
        queue.push(sys, {op = 'kill', id = id})
      end
    end

  else
    -- A lifecycle event. Ours to interpret, and mostly ours to forward.
    if m.event == 'spawned' then
      if not coord then
        coord = m.id
        say(('coordinator spawned as instance %d'):format(m.id))
        queue.push(roles, {role = 'coordinator', id = m.id})
      else
        local req = table.remove(pending, 1)
        handlers[m.id] = req
        if req then
          say(('handler %d spawned for %s'):format(m.id, tostring(req.client)))
          tell({op = 'handler', id = m.id, client = req.client,
                label = req.label})
        end
      end

    elseif m.event == 'denied' then
      -- A denied spawn leaves a 'pending' entry no 'spawned' will ever pop, so
      -- without this every later handler is attributed to the wrong client.
      -- Popping the front is order-correlation and is right only while one
      -- request is outstanding; a real one wants a token in the request that the
      -- event echoes back. 9.2's events do not carry one.
      local req = table.remove(pending, 1)
      if req then
        say(('spawn for %s refused by the runtime: %s'):format(
            tostring(req.client), tostring(m.detail)))
        tell({op = 'refused', client = req.client, detail = tostring(m.detail)})
      else
        say(('a request was denied: %s'):format(tostring(m.detail)))
      end

    elseif m.id == coord then
      -- The coordinator itself stopped.
      if m.event == 'exited' then
        say('coordinator exited cleanly; nothing left to supervise')
        return 0
      end
      restarts = restarts + 1
      say(('coordinator %s (%s); restart %d'):format(
          m.event, tostring(m.detail):sub(1, 40), restarts))
      coord = nil
      if restarts <= 3 then
        spawn_coordinator()
      else
        say('coordinator will not stay up; giving up')
        return 0
      end

    else
      -- A handler stopped. The sessions outlive a coordinator restart now, so
      -- this is news the coordinator needs whether or not it is the same
      -- coordinator that asked for the handler.
      local req = handlers[m.id]
      handlers[m.id] = nil
      if m.event == 'exceeded' then
        say(('handler %d (%s) blew its budget -- %s'):format(
            m.id, tostring(req and req.client), tostring(m.detail):sub(1, 40)))
      end
      tell({op = 'gone', id = m.id, event = m.event,
            client = req and req.client, detail = tostring(m.detail)})
    end
  end
end
