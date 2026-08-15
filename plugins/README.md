# Plugins

A plugin is a capability that lives in another program. The host execs it,
hands it one end of a socketpair as fd 3, and speaks a framed msgpack
request/reply protocol over it. `doc/BUILD8.md` §2 is the contract; this
directory is what it looks like in practice.

The point of the arrangement is what the host *does not* have to learn. The
`rest` plugin below speaks HTTP and TLS; `diluvium-host` links neither, resolves
no names, and opens no outbound socket. Replacing what a deployment can reach
does not mean replacing Diluvium.

## What is here

| Path | What it is |
|---|---|
| `dvplug.h` | A single-header, dependency-free kit for writing a plugin in C. Copy it; it is a starting point, not a dependency. |
| `rest/rest_plugin.c` | Outbound HTTP/HTTPS. Links OpenSSL. |
| `rest/rest_plugin.mjs` | The same capability in JavaScript: fd 3 under Node, `postMessage` inside a Worker. |
| `rest/rest.plugin.json` | The manifest both implementations answer to. |

`test/plugin_echo.c` is a fourth example and the smallest one. It implements the
protocol from scratch in about sixty lines and includes **nothing** from this
tree — no `dvplug.h`, no Diluvium header, no linking against the runtime. That
is deliberate and it is a test, not a demo: if a plugin ever *needs* a Diluvium
header, the channel has stopped being a protocol and become a C API.

## Wiring one up

The manifest travels with the plugin and is its author's document:

```json
{
  "schema": 1,
  "plugin": {
    "name": "rest",
    "exec": "/usr/local/libexec/diluvium-rest-plugin",
    "checksum": "sha256:…",
    "transport": "socketpair",
    "max_inflight": 8
  },
  "capabilities": [
    { "name": "get", "wake": "reissue", "args": { … }, "result": { … } }
  ]
}
```

The deployment's `plugins` block is the operator's, and says which plugins
*this* deployment wires and how hard it will lean on them:

```lua
plugins = {
  rest = { manifest = "rest.plugin.json", max_inflight = 8,
           call_timeout_ms = 15000 },
},
caps = { "queue:*", "host:rest/*" },
```

The table key becomes the call's first segment, so this answers `rest/get`,
gated by `host:rest/get` like every other hostcall and attenuating through
spawns like everything else. A guest reaches it with no new surface at all:

```lua
local r = host.call("rest/get", { url = "https://api.example.com/v1/thing" })
print(r.status, r.body)
```

## Writing one

Any language that can read a file descriptor and encode msgpack. In C, with
`dvplug.h`:

```c
#define DVPLUG_IMPL
#include "dvplug.h"

static void on_call (dv_req *q) {
  char who[256];
  if (!dv_arg_str(q, "name", who, sizeof(who))) {
    dv_error(q, DV_ERR_PLUGIN, "bad_args", "this call needs a 'name'");
    return;
  }
  dv_ok_begin(q, 1);
  dv_key(q, "greeting"); dv_str(q, who);
  dv_send(q);
}

int main (void) { return dv_serve(on_call); }
```

Four rules, each with a failure behind it:

1. **Answer every frame exactly once.** A frame you return from without
   answering leaves the calling guest waiting out its own timeout, and the
   host's ledger holding a slot it cannot reclaim until the deadline.
2. **Never write to stdout or stderr as protocol.** They are yours to log on —
   that is *why* the channel is fd 3 and not stdin/stdout. A stray `print`, a
   library log line or a panic trace on the channel desyncs the framing
   permanently, and the failure looks like corruption rather than like the log
   line it is.
3. **Pick the right error class.** `transport` means the channel or your
   process failed, `plugin` means your program is wrong, `capability` means the
   service behind you said no. The guest's retry decision differs for each,
   which is the entire reason the protocol carries a class and not a string.
4. **A non-2xx is an answer, not an error.** The guest asked what the service
   said; 404 is what it said. Reserve the error path for what you could not do.

## `wake`: the one field with no default

Every capability declares `reissue`, `cached` or `error`. It is what happens to
a call that was in flight when its instance hibernated — the host-side pending
state is outside the sandbox and cannot ride the snapshot, so *something* has to
decide, and only the capability knows whether asking twice is safe.

`rest/get` is `reissue` because a GET is idempotent. `rest/post` is `error`,
because it is not and the host cannot know whether the first one landed. There
is no default because no default is right for everything; the manifest is
refused without it.

## Building the rest plugin

```sh
make build_plugin_rest         # dist/diluvium-rest-plugin (OpenSSL)
make build_plugin_rest_notls   # http:// only, no OpenSSL
make build_plugin_rest_musl    # static, to sit beside diluvium-host-musl
```

The musl target is meant to run inside the same Alpine container
`host/build-musl.sh` drives, where `gcc` is musl-gcc and `openssl-libs-static`
provides `libssl.a`. That the *plugin* carries that dependency and the host
carries none is what keeps `build_host_musl`'s fully-static link clean.

For JavaScript, `rest/rest_plugin.mjs` needs no build. Point a manifest's `exec`
at it (it has a shebang and needs `+x`) and the native host will run it under
Node exactly as it runs the C one — `make host_check` runs the identical
end-to-end scenario through both and asserts a guest cannot tell them apart.
Inside a Worker, import it and it speaks the same frame bodies over
`postMessage`.

One difference belongs in a deployment's thinking rather than in a surprise: a
browser cannot set the `Host` header, cannot present a client certificate, and
is subject to CORS. Both bindings implement one manifest at the frame level;
the reachable URL set is not the same.

## Security posture

There is no plugin authentication, deliberately (`doc/BUILD8.md` §2.7).
Containment comes from a plugin being a narrow program — fixed endpoint, parses
a frame, does one job — not from certifying it. An attacker who can swap the
plugin binary can equally swap `diluvium-host`.

What the host does do, because each costs one line: it `execv`s an **absolute**
path and never searches `PATH`, it logs the manifest's checksum at startup
without enforcing it, and the channel has no filesystem name and no port, so
nothing else can connect to it. Parentage is structural rather than negotiated,
which is why there is nobody to authenticate.
