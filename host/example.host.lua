---An example deployment: everything the generic host can wire, wired --
---except `exec`, left commented because granting it is leaving the sandbox.
---Copy, delete what you do not need, and remember the two rules the host
---enforces harder than an editor can: an unknown key is refused by name,
---and this file cannot compute -- it is evaluated with nothing in scope.
---@type diluvium.HostConfig
return {
  supervisor = "supervisor.lua",

  max_instances = 64,
  spawns_per_step = 4,

  -- Stamps every snapshot this swarm takes; a foreign or missing stamp is
  -- refused at wake. Omit for a single-process deployment.
  identity = "example/dev",

  -- "off" is policy for a deployment that keeps agent state at the
  -- application level and wants swap-out refused by name.
  hibernation = "on",

  -- The root's ceiling. Everything any descendant will ever hold is a
  -- subset of this list, and the host:* names gate hostcalls the same way
  -- queue:* names gate queues.
  caps = { "lifecycle", "queue:*", "host:time", "host:sql/query",
           "host:crypto/*", "host:fs/*" },

  budget = { instructions = 5000000, memory_kb = 512 },

  connectors = {
    time = true,
    listen = {
      port = 8080,               -- the LB terminates TLS and speaks here
      queue = "http_in",         -- {conn, method, path, body} arrives here
      reply_queue = "http_out",  -- {conn, status, body, content_type?} leaves
      deadline_ms = 10000,       -- the host's own timeout, per connection
      headers = { "authorization" },  -- lowercase allowlist; matches arrive
                                 -- as a `headers` map on each request
    },
    sql = {
      scope = "data",            -- a directory; the program names its
                                 -- database inside it (host.sql.open)
      access = "read",           -- sql/exec stays unwired; the grant above
                                 -- only names sql/query anyway
      max_result_rows = 1024,
    },
    crypto = {
      key_file = "signing.key",  -- the deployment shape; the key never
                                 -- enters a guest
      default_ttl = 3600,
    },
    fs = {
      scope = "data",            -- files, under the same discipline as sql
      access = "read",           -- fs/write stays unwired
      max_bytes = 1048576,
    },
    -- exec = true,              -- LEAVING THE SANDBOX; uncomment knowingly
                                 -- (or a table: { max_timeout_ms = 10000,
                                 -- max_output_bytes = 1048576 })
  },
}
