# Adding a capability

A hostcall is a program asking its host for something the sandbox does not
contain. There are three ways to add one, and which you want depends less on
what the capability does than on **where the code that does it has to live**.

| route | the capability lives in | reach for it when |
|---|---|---|
| **A connector, in the C host** | `diluvium-host` itself | it is small, has no dependencies you would not link into the host anyway, and every deployment wants it |
| **A plugin** | a separate program | it needs a library, a language, or a lifetime the host should not take on — or you want to replace it without replacing Diluvium |
| **A connector, in Lab** | a JavaScript function in the page | you are in the browser, where there is no subprocess and no socket |

All three answer the same protocol. `doc/Host.md`'s acceptance test is that a
guest must not be able to tell two hosts apart, and none of these routes is a
simulation of a hostcall — they *are* hostcalls, answered by different code.
The request encoding, the capability check, the correlation token and the
status vocabulary are identical.

The guest side never changes:

```lua
local r = host.call("knob/get")
```

---

## 1. A connector in the C host

The interface is one function pointer (`host/dhost.h`):

```c
typedef dh_call_status (*dh_call_fn) (void *ud, dvs_id id, int64_t tok,
                                      const char *call,
                                      const unsigned char *args, size_t argslen,
                                      dh_buf *value,
                                      char *detail, size_t detailcap);
```

You get the call name, the arguments as raw msgpack, and a buffer to append
your answer to. You return `DH_CALL_OK`, `DH_CALL_ERROR`, `DH_CALL_DENIED`,
or — new in build 8 — `DH_CALL_PENDING`, which means *taken, I owe you
exactly one `dh_reply` later*.

By the time you are called, two things have already happened: the request has
parsed, and the capability check has passed. A connector only ever sees calls
the caller is entitled to make.

```c
static dh_call_status conn_greet (void *ud, dvs_id id, int64_t tok,
                                  const char *call,
                                  const unsigned char *args, size_t argslen,
                                  dh_buf *value, char *detail,
                                  size_t detailcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  char who[128];
  (void)ud; (void)id; (void)tok;

  if (strcmp(call, "greet/hello") != 0) {
    snprintf(detail, detailcap, "the greet connector answers 'greet/hello'");
    return DH_CALL_ERROR;
  }
  who[0] = '\0';
  diluvium_mp_open(&c, args, argslen);
  if (diluvium_mp_field(&c, "name") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_STR && t.len < sizeof(who)) {
    memcpy(who, t.p, t.len);
    who[t.len] = '\0';
  }
  if (who[0] == '\0') {
    snprintf(detail, detailcap, "greet/hello needs a 'name' string");
    return DH_CALL_ERROR;
  }
  dh_map(value, 1);
  dh_str(value, "greeting");
  {
    char out[192];
    snprintf(out, sizeof(out), "hello, %s", who);
    dh_str(value, out);
  }
  return DH_CALL_OK;
}
```

Register it in `dh_host_open`, saying what it answers so discovery can report
it properly — the router matches only the prefix, and a listing that said
`greet` where a caller needs `greet/hello` is a shape of answer nobody can
act on:

```c
static const char *const CALLS_GREET[] = { "greet/hello", NULL };
dh_register_full(h, "greet", conn_greet, NULL, CALLS_GREET,
                 DH_VIS_INHERIT, NULL);
```

Add the source to `HOST_SRCS` in the `Makefile` if it is a new file, grant
`host:greet/hello` in a deployment, and it is reachable.

**Answering later.** If the work takes time, take the call and return:

```c
  if (dh_defer(h, id, tok, my_handle, dh_now_ms() + 5000) != 0) {
    snprintf(detail, detailcap, "the host would not take this call");
    return DH_CALL_ERROR;
  }
  return DH_CALL_PENDING;
```

and settle it whenever the answer arrives:

```c
  dh_reply(h, id, tok, DH_CALL_OK, value_bytes, value_len, NULL);
```

The debt is the contract — one reply, never zero, never two. The host keeps a
ledger of what is owed, sweeps it when an instance dies, and reclaims an
entry whose deadline passes so a wedged connector cannot hang a guest. Your
descriptors join the host's single `poll()` through `dh_plug_arm` /
`dh_plug_fire` (`host/dhost_plugin.c` is the worked example).

---

## 2. A plugin

Same capability, different address space. The host execs your program from an
absolute path, hands it one end of a socketpair as fd 3, and speaks
length-prefixed msgpack frames over it (`doc/BUILD8.md` §2.4).

```c
#define DVPLUG_IMPL
#include "dvplug.h"

#include <string.h>

static void on_call (dv_req *q) {
  char who[128];
  if (strcmp(q->target, "hello") != 0) {
    dv_error(q, DV_ERR_PLUGIN, "bad_target", "this plugin answers 'hello'");
    return;
  }
  if (!dv_arg_str(q, "name", who, sizeof(who))) {
    dv_error(q, DV_ERR_PLUGIN, "bad_args", "hello needs a 'name' string");
    return;
  }
  dv_ok_begin(q, 1);
  dv_key(q, "greeting");
  {
    char out[192];
    snprintf(out, sizeof(out), "hello, %s", who);
    dv_str(q, out);
  }
  dv_send(q);
}

int main (void) { return dv_serve(on_call); }
```

```sh
cc -O2 -o /usr/local/libexec/greet greet.c     # no Diluvium headers, no linking
```

`plugins/dvplug.h` is a copyable single-header kit, not a dependency of the
runtime. `test/plugin_echo.c` implements the same protocol from scratch in
about sixty lines, deliberately, so that *"a plugin needs the protocol and
not a header"* stays checkable.

Then a manifest, `greet.plugin.json`:

```json
{
  "schema": 1,
  "plugin": {
    "name": "greet",
    "exec": "/usr/local/libexec/greet",
    "transport": "socketpair",
    "max_inflight": 4
  },
  "capabilities": [
    { "name": "hello", "wake": "reissue" }
  ]
}
```

and a deployment that wires it:

```lua
plugins = {
  greet = { manifest = "greet.plugin.json" },
},
caps = { "host:greet/hello" },
```

Three things the manifest makes you say, each because the default would be
wrong for something:

- **`exec` must be absolute.** The host `execv`s it and never searches
  `PATH`. A plugin path comes from a manifest an operator wrote; resolving it
  through the environment is an injection surface with nothing on the other
  side of the trade.
- **`wake` is required.** `reissue`, `cached` or `error` — what happens to a
  call in flight when its instance hibernates. The host-side pending state
  lives outside the sandbox and cannot ride the snapshot, so *something* has
  to decide, and only the capability knows whether asking twice is safe.
  *(Declared and validated today; nothing consumes it yet.)*
- **`max_inflight`** bounds how many frames the host writes before waiting.
  Without it, forty parked instances fill the pipe at ~64KB and the host's
  `write` blocks — the exact failure deferral exists to remove, reintroduced
  at the last possible moment.

Four rules for the program itself, each with a failure behind it:

1. **Answer every frame exactly once.** A frame you return from without
   answering leaves the caller waiting out its own timeout.
2. **Never write protocol to stdout or stderr.** They are yours to log on —
   that is *why* the channel is fd 3. A stray `print` on the channel desyncs
   the framing permanently, and it looks like corruption rather than like the
   log line it is.
3. **Pick the right error class.** `transport` (the channel or your process
   failed), `plugin` (your program is wrong), `capability` (the service
   behind you said no). The caller's retry decision differs for each, which
   is the entire reason the protocol carries a class and not a string.
4. **A non-2xx is an answer, not an error.** The guest asked what the service
   said; 404 is what it said. Reserve the error path for what you could not do.

---

## 3. A connector in Lab

There is no subprocess in a browser tab and no socket, so in Lab a capability
is a JavaScript function. Lab's `_answer` (`src/kernel/swarm.js`) is a
near-exact mirror of the C host's `answer()` — same rules, same status
vocabulary, same order — so the contract is the one above with the C removed:

```js
// src/kernel/connectors.js keeps these helpers:
//   const ok      = (value)  => ({ status: 'ok', value });
//   const denied  = (detail) => ({ status: 'denied', detail });
//   const failed  = (detail) => ({ status: 'error',  detail });

function greetConnector() {
  return (call, args) => {
    if (call !== 'greet/hello') {
      return failed("the greet connector answers 'greet/hello'");
    }
    const who = args && typeof args.name === 'string' ? args.name : '';
    if (!who) return failed("greet/hello needs a 'name' string");
    return ok({ greeting: `hello, ${who}` });
  };
}
```

Wire it the way `buildConnectors` wires the others, and attach it:

```js
swarm.connect('greet', greetConnector());
```

That is the whole thing. The guest's `host.call("greet/hello", {name = "you"})`
does not change, and neither does the capability grant.

**Two Lab-specific notes**, both worth knowing before you build on it:

- A plugin in Lab needs no plugin channel. If you want isolation, a Worker
  behind `postMessage` speaking the same frame bodies gets it —
  `plugins/rest/rest_plugin.mjs` does exactly this, and the same file runs
  under Node on fd 3 against the native host.
- Lab's swarm host reaches the wasm module through three `env` imports
  (`js_host_create`, `js_host_destroy`, `js_host_drive`). Lab's own
  integration test for that path says it has never executed anywhere, and CI
  runs only the single-instance test. Run it before building on it.

---

## Discovery

Whichever route you took, a program can ask what is there:

```lua
for _, c in ipairs(host.capabilities()) do
  print(c.name, c.kind, c.granted, c.visibility)
end
--> greet/hello   plugin      true    public
--> sql/query     connector   true    public
--> sql/exec      connector   false   public    -- exists here, not mine
```

The listing reports the **menu**, not the caller's grant. `doc/Capabilities.md`
§1 keeps those apart — a capability is what a host *can* do, and "restricting
a program can never shrink it" — so an entry the caller may not use is still
an entry, marked `granted = false`. A program can then tell *"this host
cannot do that"* from *"this host can, and I may not"*, which are different
problems with different fixes.

`visibility` decides what a caller is told exists, and it is not permission:
`public` (the default — listed to everyone), `private` (listed only to
holders), `hidden` (never listed, though a holder can still call it), and
`inherit` (take the enclosing default). Set it on the deployment or per
plugin.

Discovery is itself a capability. Grant `host:capabilities/list`.

That gating is what makes an **auditing agent** expressible: an instance
holding `host:capabilities/list` and nothing else can report everything a
swarm can reach while being able to reach none of it.

---

## Which route, in practice

Start with a **plugin** unless you have a reason not to. It keeps the host
small — `diluvium-host` links no TLS, resolves no names and opens no outbound
socket, and `plugins/rest` does all three — and it means the thing you are
most likely to change is the thing you can replace without replacing
Diluvium.

Reach for a **C connector** when the capability is small enough that a
subprocess is more machinery than the feature, and general enough that every
deployment wants it. `time` is the archetype.

In **Lab**, you have one option and it is the easy one.
