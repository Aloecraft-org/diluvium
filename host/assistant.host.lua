---An assistant-shaped deployment (build10): the workload the build10
---additions exist for, wired end to end. Inbound webhooks through the
---listener, outbound LLM and API calls through the rest plugin, secrets
---in a minted vault (script/mint_vault.lua), workers through host.spawn.
---host/assistant.lua is the supervisor this file names.
---
---The bounds here are the point, not decoration: an LLM call is slow, so
---the plugin timeout and the connection deadline are raised knowingly,
---and everything else stays tight.
---@type diluvium.HostConfig
return {
  supervisor = "assistant.lua",

  max_instances = 16,
  identity = "assistant/dev",

  -- The root's ceiling; workers get subsets of this at spawn.
  caps = { "lifecycle", "queue:*", "host:time", "host:rest/*",
           "host:fs/read", "host:sql/*", "host:crypto/*",
           "host:capabilities/list" },

  connectors = {
    time = true,

    listen = {
      port = 8080,
      queue = "http_in",
      reply_queue = "http_out",
      max_body = 262144,          -- a webhook payload, not an upload
      deadline_ms = 65000,        -- must outlive one LLM round trip, or
                                  -- every slow answer becomes a 504
      headers = { "authorization" },
                                  -- the webhook's bearer, checked in-guest
      response_headers = { "cache-control", "location" },
                                  -- what a reply's `headers` map may set;
                                  -- the framing stays the host's
    },

    -- data/ holds the vault (vault.luac, minted by script/mint_vault.lua)
    -- and whatever documents the assistant reads. Read-only: the memory
    -- that changes lives in sql below, not in files.
    fs = { scope = "data", access = "read" },

    -- The assistant's memory: conversations, notes, whatever the program
    -- schemas for itself under data/.
    sql = { scope = "data", access = "readwrite", max_result_rows = 256 },

    -- Session tokens for the assistant's own callers. The key never
    -- enters a guest -- the pattern the vault docs point at.
    crypto = { key_env = "ASSISTANT_SIGNING_KEY", default_ttl = 3600 },
  },

  plugins = {
    rest = {
      manifest = "rest.plugin.json",
      max_inflight = 4,
      call_timeout_ms = 90000,    -- the host-side backstop. The default
                                  -- 30000 would reclaim healthy LLM calls;
                                  -- raising it is a decision this comment
                                  -- exists to make visible
    },
  },
}
