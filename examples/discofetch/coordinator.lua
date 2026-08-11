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

The coordinator holds NO lifecycle capability. It cannot spawn, it cannot kill,
and it is not any handler's parent -- it asks the supervisor, which owns every
instance. That costs a message round trip per admission and buys two things:

  A crash here no longer takes the sessions with it. When an instance stops, the
  swarm layer kills its subtree, so while handlers were this program's children,
  a fault in matchmaking destroyed every live pairing session.

  And there can be more than one of these. A coordinator that owned its handlers
  could not be sharded, so one request that stalled it stalled every arrival
  behind it. Nothing here is a session's owner any more, so a second coordinator
  is a second copy of this file and nothing else.

What is left in this file is only policy: who should be paired with whom.
]]

local log     = queue.declare('log',     {capacity = 64, exported = true})
local clients = queue.declare('clients', {capacity = 16})   -- host pushes arrivals
-- 'inbox' and 'outbox' already exist: 6.6 reserves them per instance and declares
-- them exported, so the zero-configuration case needs no code. Declaring one again
-- is an error rather than a silent reconfigure, which is how this was found.
local inbox   = queue.lookup('inbox')    -- handler reports, and supervisor news
local out     = queue.lookup('outbox')   -- requests to the supervisor
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
-- Factor IV's publisher seam, in the smallest form that is still real: the
-- coordinator allocates a label, not the client and not the host. Clients arrive
-- with names they chose; a label is what this swarm issued, and the question the
-- toy exists to answer is what happens to one when its handler dies.
local labels   = {}    -- instance id -> label issued to it
local nextlabel = 0
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
local function handler_for (name, want, nat, label)
  -- One pass, deliberately: see the README.
  local subs = {CLIENT = name, WANT = want, NAT = nat, LABEL = label}
  return (HANDLER_SRC:gsub('@(%u+)@', function (k)
    local v = subs[k]
    if v == nil then return nil end     -- leave unknown placeholders alone
    return ('%q'):format(v)
  end))
end

local function admit (c)
  nextlabel = nextlabel + 1
  c.label = ('dv-%04d'):format(nextlabel)
  say(('%s wants %s (nat %s) -> spawning a handler as %s'):format(
      c.client, c.want, c.nat, c.label))
  pending[#pending + 1] = c
  queue.push(out, {
    op   = 'need_handler',
    client = c.client,
    label  = c.label,
    code = handler_for(c.client, c.want, c.nat, c.label),
    --[[
    Narrower again: one queue name, for this client only, and *no* lifecycle. A
    handler therefore cannot spawn anything -- it may declare system/lifecycle and
    push to it, and nothing will ever read it.

    The 'greedy' case asks for 'queue:*', wider than the 'queue:client/*' this
    program holds -- and the refusal now comes from the *supervisor* rather than
    from the runtime, which is the price of not spawning our own children. The
    swarm layer checks a grant against the caps of whoever calls spawn, and that
    is the supervisor now, which holds the root's ceiling. See the long comment
    in supervisor.lua: it was measured, and greedy's handler really was created
    holding 'lifecycle' before that check existed.

    Nothing is created: a refused spawn costs nothing and leaves nothing behind.
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

-- All state for one instance; every lifecycle event that ends one runs it.
local function forget (id)
  local name = byid[id]
  -- The label goes when the instance does. A label outliving its handler is the
  -- publisher-seam version of the stale-id bug this function was written for.
  if labels[id] then say(('label %s released with handler %d'):format(labels[id], id)) end
  labels[id] = nil
  byid[id] = nil
  if name then idof[name] = nil end
  for k, v in pairs(waiting) do if v.id == id then waiting[k] = nil end end
end

-- Only an ancestor may kill, and this program is nobody's ancestor now. It asks.
local function kill (id)
  queue.push(out, {op = 'release', ids = {id}})
end

while true do
  -- No 'system/events' here any more. Events go to an instance's parent, and this
  -- program is nobody's parent -- the supervisor gets them and forwards what
  -- matters. So everything except a client arrival comes through 'inbox'.
  local q, m = queue.wait({clients, inbox})

  if q == clients then
    admit(m)

  elseif m.op == 'handler' then
    -- The supervisor spawned what we asked for and is telling us which id it is.
    local c = table.remove(pending, 1)
    if c then
      byid[m.id] = c.client
      idof[c.client] = m.id
      labels[m.id] = c.label
      say(('handler %d is %s, label %s'):format(m.id, c.client, c.label))
    end

  elseif m.op == 'refused' then
    -- The runtime refused the spawn. Pop the matching request, or every later
    -- handler is attributed to the wrong client -- which is order-correlation,
    -- and it is right only while one request is outstanding. A real one wants a
    -- token in the request that the answer echoes; 9.2's events carry none.
    local c = table.remove(pending, 1)
    say(('spawn for %s was refused (%s); label %s was never issued'):format(
        tostring(m.client), tostring(m.detail),
        tostring(c and c.label)))

  elseif m.op == 'gone' then
    -- A handler stopped, whatever the reason. This is the news that used to
    -- arrive as a lifecycle event when handlers were this program's children.
    if m.event == 'exceeded' then
      say(('handler %d (%s) blew its budget and was stopped -- %s'):format(
          m.id, tostring(m.client), oneline(m.detail)))
    elseif m.event == 'faulted' then
      say(('handler %d (%s) faulted: %s'):format(
          m.id, tostring(m.client), oneline(m.detail)))
    end
    forget(m.id)

  elseif m.kind == 'ready' then
    -- A handler reporting for itself, routed here by the host.
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
