--[[
handler.lua -- one client's session, and nothing else's.

This is a *template*. The coordinator substitutes @CLIENT@, @WANT@ and @NAT@ and
spawns the result, so every client gets a program written for it. Generating a
program per unit of work is the ordinary shape here rather than a trick -- 9.1 is
built around a system whose programs are produced and rewritten at runtime.

What matters about this file is what it does NOT have:

  no lifecycle capability, so it cannot spawn anything -- it may declare
  system/lifecycle and push to it, and nothing will ever read it;

  no way to reach another handler, because a program cannot push into another
  program's queue. Everything goes out through 'out', and the host decides where
  that lands;

  no shared state with any other client. The candidates below are locals in a Lua
  state that belongs to this instance. When the instance is killed they are not
  cleared, freed, or overwritten -- the state they were in stops existing.
]]

local CLIENT = @CLIENT@
local WANT   = @WANT@
local NAT    = @NAT@

local log   = queue.declare('log', {capacity = 8,  exported = true})
-- 6.6 reserves 'outbox' per instance and exports it, so this needs no declare.
local out   = queue.lookup('outbox')
-- The one queue this handler is allowed to name, per the capability it was granted.
local inbox = queue.declare('client/' .. CLIENT, {capacity = 8})

local function say (s) queue.push(log, s) end

--[[
Toy ICE. A real handler would gather host, STUN-reflexive and relay candidates and
sort them by the usual priority formula; this makes up three plausible ones so the
shape is visible. The important property is that this list exists only here.
]]
local function gather ()
  local cands = {}
  local kinds = (NAT == 'symmetric') and {'host', 'relay'}
                                      or {'host', 'srflx', 'relay'}
  for i, kind in ipairs(kinds) do
    cands[i] = {
      kind = kind,
      addr = ('10.%d.%d.%d:%d'):format(#CLIENT, i, 7, 40000 + i * 111),
      -- Not the real formula, just monotone in the way ICE priorities are.
      priority = (kind == 'host' and 126 or kind == 'srflx' and 100 or 0) * 1000 - i,
    }
  end
  return cands
end

local candidates = gather()
say(('%s: gathered %d candidates behind a %s nat'):format(
    CLIENT, #candidates, NAT))

--[[
The deliberately hostile client.

Its handler never yields, so nothing cooperative can stop it -- which is exactly
9.4's argument for putting the limit outside the guest. The instruction budget the
coordinator set fires, the instance dies with 'exceeded', and the coordinator hears
about it. Every other handler is untouched.

A guest can of course also just fail. Both paths end with the parent being told,
which is why 'exited', 'faulted' and 'exceeded' are three separate events rather
than one "it stopped".
]]
if CLIENT == 'runaway' then
  say('runaway: entering a loop that will not end')
  local n = 0
  while true do n = n + 1 end
end

-- Tell the coordinator we are ready. Only what it needs: who, and what for. The
-- candidates stay here until there is a peer to give them to.
queue.push(out, {kind = 'ready', client = CLIENT, want = WANT})

--[[
Park. This is where a real session spends its life, and at scale it is where
almost every instance is: idle on an inbox, costing about 42 KB and no CPU.

'queue.wait' with nothing ready parks the program and hands control back to the
host, which decides when it runs again. There is no thread here and no scheduler.
]]
while true do
  local _, m = queue.wait({inbox})
  if m and m.peer then
    -- The other side's candidates arrived; a real handler would start
    -- connectivity checks here, highest priority first.
    say(('%s: peer %s offered %d candidates; starting checks'):format(
        CLIENT, tostring(m.peer), m.candidates and #m.candidates or 0))
    queue.push(out, {kind = 'checking', client = CLIENT, peer = m.peer})
  end
end
