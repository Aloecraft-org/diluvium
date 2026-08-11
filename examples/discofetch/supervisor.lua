--[[
supervisor.lua -- the root of the swarm.

There is no supervisor type in Diluvium, and that is the design rather than an
omission (doc/Messaging.md 9.1). This is an ordinary program. The only thing that
makes it a supervisor is that it holds the "lifecycle" capability, so the swarm
layer bothers to read its system/lifecycle queue at all -- a program without that
capability may declare the queue and write to it all it likes, and nothing will
ever read it. The check is at drain time rather than at declare time on purpose:
there is no special case to catch and work around.

What lives here is *policy*: which children to make, what to grant them, how much
they may spend, and what to do when one dies. None of that is in the runtime,
which is the point -- a restart strategy you can rewrite at runtime is a program,
and a restart strategy compiled into a C library is not.
]]

local sys = queue.declare('system/lifecycle', {capacity = 8})
local ev  = queue.declare('system/events',    {capacity = 32})
local log = queue.declare('log',   {capacity = 64, exported = true})
-- How the host learns which instance became the coordinator. Ids are the swarm's
-- to assign, so a program that wants the host to route to a peer must say so.
local roles = queue.declare('roles', {capacity = 4, exported = true})
-- The host pushes the coordinator's source in here, as an ordinary message.
-- Code is data: there is nothing special about a string that happens to be a
-- program, and for a system meant to rewrite its own agents at runtime that is
-- the feature rather than a curiosity.
local codeq = queue.declare('code', {capacity = 2})

local function say (s) queue.push(log, s) end

say('supervisor up; waiting for the coordinator program')
local _, COORD_SRC = queue.wait({codeq})
say(('got the coordinator program (%d bytes)'):format(#COORD_SRC))

--[[
The spawn request, in the shape 9.1 documents.

Two things worth reading closely:

  caps -- strictly narrower than what this program holds. The root holds
  "queue:*"; the coordinator gets "queue:client/*". A trailing '*' is a prefix, so
  that is a real narrowing. 9.3 admits no exceptions: the swarm layer refuses a
  grant that is not an attenuation, so a supervisor cannot hand out authority it
  does not itself hold, even by mistake.

  budget -- nested, as 9.1 shows it. This was silently ignored until recently: the
  child ran unbudgeted and the spawn still reported success, which is the kind of
  defect that only surfaces when something runs away.
]]
local function spawn_coordinator ()
  queue.push(sys, {
    op    = 'spawn',
    code  = COORD_SRC,
    caps  = { 'lifecycle', 'queue:client/*' },
    budget = { instructions = 20000000, memory_kb = 2048 },
  })
end

spawn_coordinator()

local restarts = 0

while true do
  local _, e = queue.wait({ev})

  if e.event == 'spawned' then
    say(('coordinator spawned as instance %d'):format(e.id))
    queue.push(roles, {role = 'coordinator', id = e.id})

  elseif e.event == 'faulted' or e.event == 'exceeded' then
    -- Restart policy: a program, not a runtime feature. About as simple as a
    -- policy gets -- three tries, then stop, which is a legitimate answer and
    -- better than restarting forever.
    restarts = restarts + 1
    say(('coordinator %s (%s); restart %d'):format(
        e.event, tostring(e.detail):sub(1, 50), restarts))
    if restarts <= 3 then
      spawn_coordinator()
    else
      say('coordinator will not stay up; giving up')
      return 0
    end

  elseif e.event == 'exited' then
    say('coordinator exited cleanly; nothing left to supervise')
    return 0

  else
    say(('event: %s id=%s (%s)'):format(e.event, tostring(e.id),
        tostring(e.detail)))
  end
end
