-- assistant.lua
-- The supervisor host/assistant.host.lua names: an assistant as a swarm
-- program, written against the build10 surface. This is an example to copy
-- from, not a product -- the LLM request shape is the Anthropic Messages
-- API's, trimmed to the fields that matter here.
--
-- The shape to take away is the loop: wake on inbound HTTP, drain child
-- events with host.events(0) on the same pass, answer, park again. It is
-- deliberately SEQUENTIAL -- one request at a time, host.call parking on
-- its reply while the listener buffers the next request host-side. The
-- concurrent shape (one wait-set over http_in and the reply queue at
-- once, serving while a call is in flight) is real and tested --
-- test/host_check.c's self-fetch fixture -- but it is the second version
-- of this program to write, not the first.

local inq     = queue.declare("http_in",  { capacity = 16 })
local outq    = queue.declare("http_out", { capacity = 16, exported = true })

-- The vault: minted by script/mint_vault.lua, fetched over the fs grant,
-- compiled in place ('load' works in a sealed guest; 'loadfile' does not
-- exist here). Fetch-per-use is the documented discipline -- the getter is
-- kept, the VALUES are not: a secret held in a long-lived local rides
-- every hibernation snapshot in the clear.
local get_secret
do
  local bytes = host.fs.read("vault.luac")
  get_secret = assert(load(bytes, "=vault", "b"))()
end

-- Until rest credential injection lands (build11's headline), the key
-- transits the guest on its way into the header. The vault keeps it off
-- the config floor and out of the source; it cannot yet keep it out of
-- the instance.
local function llm (prompt)
  local r = host.call("rest/post", {
    url = "https://api.anthropic.com/v1/messages",
    headers = {
      ["x-api-key"] = get_secret("ANTHROPIC_API_KEY"),
      ["anthropic-version"] = "2023-06-01",
      ["content-type"] = "application/json",
    },
    body = json.encode({
      model = "claude-sonnet-5",
      max_tokens = 1024,
      messages = { { role = "user", content = prompt } },
    }),
    timeout_ms = 85000,           -- under the deployment's 90s backstop
  }, 88000)                       -- and the library waits past both
  -- r.headers (build10) is where the service's rate-limit and request-id
  -- headers arrive, when deciding to back off matters.
  local body = json.decode(r.body)
  return body.content and body.content[1] and body.content[1].text or ""
end

-- A worker per long job, budgeted, attenuated to what the job needs and
-- nothing else. The child works through its own grants; its exit comes
-- back as an event on the same loop below.
local function spawn_worker (job_id, prompt)
  return host.spawn{
    code = ("local job_id = %q\nlocal prompt = %q\n"):format(job_id, prompt)
        .. [[
      -- the worker's own program; it holds rest and sql, nothing else
      local r = host.call("rest/post", { url = "https://api.anthropic.com/v1/messages",
        headers = { ["content-type"] = "application/json" },
        body = prompt, timeout_ms = 85000 }, 88000)
      local db = host.sql.open("assistant.db")
      db.exec("INSERT INTO results (job, body) VALUES (?, ?)", job_id, r.body)
    ]],
    caps = { "queue:*", "host:rest/post", "host:sql/*" },
    budget = { instructions = 50000000, memory_kb = 4096 },
  }
end

-- The loop. queue.wait wakes on whichever source speaks first; a timeout
-- is the idle tick (poll the mail plugin here, when there is one).
local db = host.sql.open("assistant.db")
db.exec("CREATE TABLE IF NOT EXISTS results (job TEXT, body TEXT)")

while true do
  local id, m, why = queue.wait({ inq }, 30000)
  if why == "ok" and id == inq then
    local bearer = m.headers and m.headers.authorization
    if bearer ~= "Bearer " .. get_secret("WEBHOOK_TOKEN") then
      queue.push(outq, { conn = m.conn, status = 404, body = "" })
    elseif m.path == "/ask" then
      queue.push(outq, {
        conn = m.conn, status = 200,
        body = llm(m.body),
        content_type = "text/plain",
        headers = { ["cache-control"] = "no-store" },   -- build10: gated by
                                                        -- response_headers
      })
    elseif m.path == "/job" then
      local child = spawn_worker(tostring(host.time()), m.body)
      queue.push(outq, {
        conn = m.conn, status = 201, body = "",
        headers = { location = "/job/" .. child.id },
      })
    else
      queue.push(outq, { conn = m.conn, status = 404, body = "" })
    end
  end
  -- Children's exits, faults and budget overruns, drained without a second
  -- place to sleep -- host.events(0) polls the buffer and the queue.
  local ev = host.events(0)
  while ev do
    if ev.event == "faulted" or ev.event == "exceeded" then
      db.exec("INSERT INTO results (job, body) VALUES (?, ?)",
              "child:" .. tostring(ev.id), ev.event)
    end
    ev = host.events(0)
  end
end
