--[[
coordinator.lua -- matchmaking, and one instance per client.

This is the part that would be Discofetch proper. A client arrives; the
coordinator gives it a *handler instance of its own*, generated for it, holding
only that client's data. Two clients wanting the same thing get matched, told
about each other, and their handlers are killed -- and at that point the swarm has
forgotten they were ever here.

That is the security story, and it is structural rather than diligent: alice's
candidates are not in a table in a shared process that someone might read or fail
to clear. They are in a Lua state that only alice's handler can reach, and the
state is gone when the handler is.

The coordinator holds "lifecycle" too, so it spawns its own children. That makes
the handlers *its* subtree -- so if the coordinator dies, its handlers die with it
(9.5's subtree default), and the supervisor restarting it starts from an empty
world rather than adopting orphans.
]]

local sys     = queue.declare('system/lifecycle', {capacity = 32})
local ev      = queue.declare('system/events',    {capacity = 64})
local log     = queue.declare('log',     {capacity = 64, exported = true})
local clients = queue.declare('clients', {capacity = 16})   -- host pushes arrivals
-- 'inbox' and 'outbox' already exist: 6.6 reserves them per instance and declares
-- them exported, so the zero-configuration case needs no code. Declaring one again
-- is an error rather than a silent reconfigure, which is how this was found.
local inbox   = queue.lookup('inbox')                       -- host routes replies here
local codeq   = queue.declare('code',    {capacity = 2})

local function say (s) queue.push(log, s) end

-- A fault detail is a Lua error with a traceback, so it arrives with newlines in
-- it. One line is what a log wants.
local function oneline (s)
  return (tostring(s):gsub('%s*\n.*', ''))
end

say('coordinator up; waiting for the handler template')
local _, HANDLER_SRC = queue.wait({codeq})
say(('got the handler template (%d bytes)'):format(#HANDLER_SRC))

-- What the coordinator retains. Deliberately tiny: a name, what they want, and
-- which instance is theirs. No candidates, no addresses, no session data -- those
-- live in the handler and never come here.
local waiting  = {}    -- want -> { client = name, id = instance }
local pending  = {}    -- spawn order, so a 'spawned' event can be attributed
local byid     = {}    -- instance id -> client name
local idof     = {}    -- client name -> instance id
local matches  = 0

--[[
Why 'idof' exists, since it looks redundant.

A handler does not know its own instance id and has no way to learn one: ids are
the swarm's to assign, and the only thing told about a spawn is the *parent*, via
the 'spawned' event. So a report arriving from a handler says who it is by name,
and the coordinator is the only thing that can turn that back into an id -- which
it needs, because 'kill' takes an id.

The ordering works out: 'spawned' is emitted when the instance is created, before
it has run a single instruction, so the mapping is always in place before that
handler's first message can arrive.
]]

--[[
Generate a handler for one client.

String substitution on a template, which is the ordinary way to do this here:
a program cannot push into another program's queue, so a coordinator that wants a
handler to know which client it serves writes that into the handler's source.
The alternative is an endpoint reference forwarded in a message, which is the right
answer once traffic has to flow both ways -- see the README.

%q is doing real work: it produces a Lua-safe quoted literal, so a client name
with a quote or a newline in it cannot break out of the string and become code.
Without it this function would be an injection hole with extra steps.
]]
local function handler_for (name, want, nat)
  local src = HANDLER_SRC
  src = src:gsub('@CLIENT@', function () return ('%q'):format(name) end)
  src = src:gsub('@WANT@',   function () return ('%q'):format(want) end)
  src = src:gsub('@NAT@',    function () return ('%q'):format(nat) end)
  return src
end

local function admit (c)
  say(('%s wants %s (nat %s) -> spawning a handler'):format(c.client, c.want, c.nat))
  pending[#pending + 1] = c
  queue.push(sys, {
    op   = 'spawn',
    code = handler_for(c.client, c.want, c.nat),
    -- Narrower again: one queue name, for this client only, and *no* lifecycle.
    -- A handler therefore cannot spawn anything -- it may declare
    -- system/lifecycle and write to it, and nothing will ever read it.
    --[[
    Narrower again: one queue name, for this client only, and *no* lifecycle. A
    handler therefore cannot spawn anything -- it may declare system/lifecycle and
    push to it, and nothing will ever read it.

    The 'greedy' case asks for 'queue:*', which is WIDER than the 'queue:client/*'
    this coordinator holds. 9.3 admits no exceptions, so the swarm layer refuses
    the spawn outright and says which capability it refused. Nothing is created --
    a denied spawn costs nothing and leaves nothing behind.
    ]]
    caps = (c.client == 'greedy') and { 'queue:*' }
                                  or  { 'queue:client/' .. c.client },
    -- Small on purpose. A handler that will not stop is stopped for it, and the
    -- coordinator hears 'exceeded' rather than the swarm hanging. 9.4: abort, not
    -- schedule -- a guest cannot meaningfully limit itself, because a runaway loop
    -- never yields and nothing cooperative will stop it.
    budget = { instructions = 500000, memory_kb = 256 },
  })
end

local function kill (id)
  queue.push(sys, {op = 'kill', id = id})
end

while true do
  local q, m = queue.wait({clients, inbox, ev})

  if q == clients then
    admit(m)

  elseif q == ev then
    if m.event == 'spawned' then
      local c = table.remove(pending, 1)
      if c then
        byid[m.id] = c.client
        idof[c.client] = m.id
        say(('handler %d is %s'):format(m.id, c.client))
      end

    elseif m.event == 'exceeded' then
      idof[byid[m.id] or ''] = nil
      -- The runaway client. Its handler burned its instruction budget and was
      -- stopped; the coordinator is told, and the rest of the swarm is unaffected.
      say(('handler %d (%s) blew its budget and was stopped -- %s'):format(
          m.id, tostring(byid[m.id]), oneline(m.detail)))
      byid[m.id] = nil

    elseif m.event == 'faulted' then
      say(('handler %d (%s) faulted: %s'):format(
          m.id, tostring(byid[m.id]), oneline(m.detail)))
      byid[m.id] = nil

    elseif m.event == 'exited' then
      byid[m.id] = nil

    elseif m.event == 'denied' then
      say(('a request was denied: %s'):format(tostring(m.detail)))

    elseif m.event == 'throttled' then
      -- 9.5's rate limit as a rate rather than a filter: the request stays queued
      -- and arrives over the next few steps. Nothing to do but notice.
      say('spawns are being throttled; the queue will drain')
    end

  else  -- inbox: a handler reported, routed here by the host
    if m.kind == 'ready' then
      local me = idof[m.client]
      local other = waiting[m.want]
      if other and other.client ~= m.client and me then
        matches = matches + 1
        waiting[m.want] = nil
        say(('MATCH #%d on %s: %s <-> %s'):format(
            matches, m.want, other.client, m.client))
        -- In the real thing, each side is handed the other's candidates here and
        -- the two clients talk directly. The point of the toy is what happens
        -- next: both handlers are killed, and with them goes every byte either
        -- client sent. Nothing is retained to be leaked later.
        say(('  both handlers (%d, %d) are killed; their state goes with them')
            :format(other.id, me))
        kill(other.id)
        kill(me)
      else
        waiting[m.want] = {client = m.client, id = me}
        say(('%s is waiting for a peer on %s'):format(m.client, m.want))
      end
    end
  end
end
