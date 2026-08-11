# Diluvium

<div align="center">

<img src="doc/aloecraft_logo.png" style="height:96px; width:96px;"/>

**Lua for Modern Development**

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Repo](https://img.shields.io/badge/github-repo-blue.svg)](https://github.com/Aloecraft-org/diluvium)
[![Lua Version](https://img.shields.io/badge/lua-5.5.1-purple.svg)](https://www.lua.org/)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Ready-orange.svg)](https://webassembly.org/)

[Try it Online](https://diluvium.aloecraft.org) | [Documentation](https://www.lua.org/docs.html)

</div>

## What is Diluvium?

The Diluvium Programming Language is a **100% backward-compatible** extension of Lua with modern features like string interpolation, null coalescing, security enhancements, and more, without breaking legacy code. It's blazing fast, tiny (~1MiB runtime), and runs just about everywhere.

## Language Features

**String Interpolation**
```lua
-- Standard Lua works perfectly
local function greet(name)
    return "Hello, " .. name
end

-- So does string interpolation
function greet_modern(name)
    -- String interpolation
    return $"Hello, {name}!"
end
```

**Null Coalescing**
``` lua
-- And null coalescing
local config = user_config ?? default_config

print(nil ?? "hello!")
```

**Secure Functions**

**NOTE:** Secure functions are cryptographically weak but their contents cannot be read by simply opening a text editor.

``` lua
-- Use a tilde (~) to make a function into a secure function*
~function secure_function()
    -- Variable names and variable constants will be encrypted at rest
    password="BUY_SHIB_lol"

    -- That include string literals
    return $"My password is, {password}!"
end
``` 

A compiled chunk stores each distinct string once, so a literal a secure
function shares with ordinary code has a single copy — and it is the
secure function that decides, meaning that copy is hidden everywhere it
is used. Nothing a secure function contains appears in the dump in the
clear, whatever else in the chunk happens to use it.


**Switch**

``` lua
-- The subject is evaluated once, a case can list several values,
-- and there is no fallthrough
switch response_code do
    case 200, 201, 204 then
        print("ok")
    case 301, 302 then
        print("redirected")
    default
        print($"unexpected: {response_code}")
end
```

`switch` is a contextual keyword, so existing code that uses it as a
variable, field or function name keeps working. The one consequence is
that a subject cannot start with `(`, a string or a table constructor --
`switch (x)` is a function call in stock Lua and stays one. Write
`switch x do` (the usual Lua shape, as in `if x then` and `while x do`),
or bind the subject to a local first.

**Compound Assignment**

``` lua
local total = 0
total += 10        -- and -=  *=  /=  //=  %=  ^=
total *= 2
local name = "dil"
name ..= "uvium"   -- concatenation
local flags = 0
flags |= 0x04      -- and &=  <<=  >>=
local port = nil
port ??= 8080      -- assign only when nil
```

The target's prefix is evaluated once, so `t[next_key()] += 1` calls
`next_key` a single time. There is no `~=` form, because `~=` already
means "not equal".

**Defer**

``` lua
local f = io.open("data.txt")
defer f:close()          -- runs however the block exits

do
    defer do             -- a block, when one statement is not enough
        print("cleaning up")
        release(handle)
    end
    risky()              -- even if this raises
end
```

Deferred statements run in reverse order when the block ends, and on
`break`, `goto`, `return`, an error, or `coroutine.close`. `defer` is a
contextual keyword, so existing code using it as a name keeps working.

**Safe Navigation**

``` lua
-- Once something is nil, the rest of the chain is skipped entirely --
-- nothing is indexed, and no call happens
local city = user?.address?.city
local name = config?.profile.display_name    -- plain '.' after '?.' is safe too
handler?.on_event(payload)                   -- not called when handler is nil

-- Pairs naturally with null coalescing
local port = config?.server?.port ?? 8080
```

`?.` and `?[` test for `nil` specifically, not for falsiness, so
`false?.x` still raises exactly as `false.x` does.

**Format Specifications**

``` lua
local pi, items = 3.14159, 42
print($"pi is {pi::%.2f}")           -- pi is 3.14
print($"[{items::%5d}]")             -- [   42]
print($"{items::%#x}")               -- 0x2a
```

Everything after `::` is handed to `string.format`. It is `::` rather
than `:` because `:` already means a method call, and `$"{obj:method()}"`
keeps meaning exactly that.

**With**

``` lua
with f = assert(io.open("data.txt")) do
    for line in f:lines() do process(line) end
end                      -- f is closed here, however the block is left
```

Each binding is a to-be-closed local scoped to the block, so the value
must have a `__close` metamethod. Several bindings close in reverse order.

**And coming soon:** 
- decimal literals and a literal-suffix registry
- and more

## Why Diluvium?

🚀 **Lightweight & Blazing Fast**

The entire runtime is less than 1 MiB. Compiles to WebAssembly, x86_64, ARM64, and more, it launches instantly and runs anywhere.


🏃🏽‍♀️‍➡️ **Try Diluvium in Your Browser**

See it in action at [diluvium.aloecraft.org](https://diluvium.aloecraft.org/#terminal)

🔄 **Lua Ecosystem Compatible**

- Works with existing Lua libraries and tools
- Standard Lua code runs unmodified
- Gradual adoption—use new features only where they help

---

## Quick Start

**[`doc/Guide.md` is the programmer's guide](doc/Guide.md)** — the language additions,
the `msgpack`, `queue` and `endpoint` libraries, the shape of a program that parks on a
queue, embedding an instance from C, and running a swarm of them. Every sample in it
was run against the tree.

For a worked example, `examples/discofetch/` is a supervisor, a coordinator and one
instance per connected client, with a Dockerfile:

```sh
make -C examples/discofetch run
```

The rest of `doc/` is about building Diluvium rather than using it:
`doc/Messaging.md` is the messaging and swarm design (and §18 the known defects),
`doc/ROADMAP.md` the language and compiler state, `doc/Lab.md` the brief for a REPL
and debugger, `doc/Determinism.md` an open design for a replayable scheduler, and
`doc/audit/` the evidence behind §18.

## Installation Instructions

**Linux (Portable AMD64) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_linux_static_x86_64
cp diluvium_linux_static_x86_64 diluvium && chmod +x diluvium
./diluvium
```

**Linux (aarch64/Raspberry Pi 3/4/5) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_linux_static_aarch64
cp diluvium_linux_static_aarch64 diluvium && chmod +x diluvium
./diluvium
```

**Linux (32bit armv7l/Raspberry Pi 1/2/Zero/Zero W) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_linux_static_armv7l
cp diluvium_linux_static_armv7l diluvium && chmod +x diluvium
./diluvium
```

**MacOS (ARM64) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_darwin_arm64
cp diluvium_darwin_arm64 diluvium && chmod +x diluvium
./diluvium
```

**MacOS (Intel) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_darwin_x86_64
cp diluvium_darwin_x86_64 diluvium && chmod +x diluvium
./diluvium
```

**Windows Installation**
``` ps
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.5.1_build1/diluvium_windows_x86_64.exe -OutFile diluvium.exe
./diluvium
```

## Interactive Mode

``` sh
diluvium          # syntax highlighting, tab completion, persistent history
diluvium -h       # the options
```

Tab completes names, including through tables and metatable `__index`, so
`string.f` offers `find` and `format`. The arrow keys walk history kept in
`~/.diluvium_history`. Type `help()` for what Diluvium adds to Lua and what
the editor's keys do. Set `NO_COLOR` to turn highlighting off.

Line editing is built in — Diluvium links no readline and depends on
nothing but libc.

## The WASM build

`doc/repl-reference.html` is a working Diluvium REPL in a browser
terminal, and the reference for driving the WASM build: load
`libdiluvium_wasi.wasm`, call `__wasm_call_ctors`, then `repl_eval` for
evaluation (it reports whether input is merely unfinished) and
`repl_complete` for completion.

**What the artifact contains changed at `5.5.1_build3`, and nothing said
so.** `src/onelua.c` gained `dqueue.c`, `dendpoint.c`, `dmsgpack.c` and
`dv.c`, and `src/wasm_stubs.c` began calling `diluvium_openlibs`. Those
three edits landed in one release and took the browser build from plain
Lua to carrying the instance ABI and the guest messaging libraries.

| in `libdiluvium_wasi.wasm` | 5.4.7_release | 5.5.1_build1 | _build2 | _build3 |
|---|:---:|:---:|:---:|:---:|
| `repl_eval`, `repl_complete` | yes | yes | yes | yes |
| `dv_*` — the instance ABI, 27 exports | — | — | — | **yes** |
| `queue`, `endpoint`, `msgpack` as guest globals | — | — | — | **yes** |
| `dvs_*` — the swarm layer | — | — | — | — |

Measured rather than inferred: each published artifact was downloaded,
read with `WebAssembly.Module.exports`, and then asked `type(queue)` in a
running state.

The `dv_*` set is the whole of `dv.h` — `dv_new`, `dv_load`, `dv_run`,
`dv_resume`, `dv_snapshot`, `dv_restore`, the `dv_queue_*` and
`dv_endpoint_*` families, `dv_set_budget`, `dv_usage`, `dv_exceeded`,
`dv_waitset_get`, `dv_layout`, `dv_register_code`. So a browser host can
run sandboxed instances under an instruction budget and drain their
queues, which is what `doc/Messaging.md` §12.3's npm sketch describes. In
an ordinary `run_lua` state, `queue.declare`, `push`, `len`, `capacity`
and the non-blocking `pop` all work; only the blocking `queue.wait` needs
a host that resumes, which a bare state is not.

`dvs_*` is in no published artifact and will stay that way until somebody
decides otherwise. `dvs.c` is deliberately outside the amalgamation and
the wasi link line is `onelua.o + wasm_stubs.o + analyze.o +
diluvium_api.o` (`Makefile:199`), so the browser has every tier except
the swarm. That is not a decision about wasm — it is a side effect of one
about the amalgamation. See `doc/Lab.md` §1.

## Compiler Features

**Bytecode analysis report**

The `-r` flag generates a compiler analysis report. This comes in handy working with bytecode compiled from secure functions.

``` sh
diluvium_compiler -r -o test_analysis.rpt ./test/test_analysis.lua
```

Example Output:
``` json
{
  "functions": [
    {
      "name": "@./test/secure_function.lua",
      "params": 0,
      "vararg": true,
      "upvalues": ["_ENV"],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:2",
      "params": 2,
      "param_names": ["a", "b"],
      "vararg": false,
      "upvalues": ["_ENV"],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:8",
      "params": 1,
      "param_names": ["name"],
      "vararg": false,
      "upvalues": [],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:15",
      "params": 0,
      "vararg": false,
      "upvalues": ["_ENV"],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:23",
      "params": 0,
      "vararg": false,
      "upvalues": ["_ENV"],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:26",
      "params": 0,
      "vararg": false,
      "upvalues": ["_ENV"],
      "returns_table": false
    },
    {
      "name": "@./test/secure_function.lua:34",
      "params": 0,
      "vararg": false,
      "upvalues": ["_ENV"],
      "returns_table": false
    }
  ],
  "globals_after_load": {
    "functions": ["SecureAdd", "SecureGreet", "SecureSecret", "OuterSecure"],
    "variables": ["NormalFunction", "InnerNotSecure"]
  }
}
```
## Loading bytecode you did not compile

**Compiled chunks from a source you do not trust are not safe to run.**
This is Lua's position as much as Diluvium's — Lua has shipped no
bytecode verifier since 5.2 — but it is worth stating plainly here,
because secure functions and the analysis report exist precisely so that
a chunk somebody else compiled can be handed to you.

A corrupt or hostile chunk can reach memory it should not: measured with
`script/fuzz_exec.py`, roughly 7% of single-byte-mutated chunks crash the
interpreter when run, and about a fifth of those are out-of-bounds heap
*writes*. Instructions carry register, constant and upvalue indices that
nothing currently checks against the prototype that owns them, so treat
running untrusted bytecode as equivalent to running untrusted native
code.

The complete mitigation is Lua's own `mode` argument, which refuses
binary chunks outright:

``` lua
local f, err = load(untrusted, "chunk", "t")   -- "t" = text only
-- => nil, "attempt to load a binary chunk (mode is 't')"
```

Inspecting is safer than running. `diluvium_compiler -r` loads a chunk
and describes it without executing it, and the loader itself survives
mutation testing — 30,000 mutated chunks, no crash. That is the intended
way to look at something before you decide to trust it. It is not a
guarantee: safer is not safe.

A load-time verifier that bounds-checks instruction operands is the fix
and is the largest open item on the runtime; `doc/ROADMAP.md` tracks it.
Source `.lua` files are unaffected by any of this.

## Compatibility

What this fork promises, so that "stable" means something specific:

**Source compatibility with stock Lua is absolute.** Every construct
Diluvium adds is a syntax error in stock Lua, and none of them takes a
reserved word — `switch`, `case`, `default`, `defer` and `with` are
contextual keywords and stay usable as ordinary names. Lua code runs
unmodified. This will not change.

**Diluvium's own syntax is settled for the 5.5 line.** The constructs
here are what 5.5 ships; new ones may be added, but what exists keeps its
meaning.

**Bytecode format may change between builds.** A compiled chunk carries a
format byte and loads only into a build carrying the same one, so a stale
chunk is refused rather than misread — recompile and carry on. The
current format is `0x46`; the next change is expected when decimal
literals land. Source is never affected.

**The C API is Lua's**, plus what `diluvium_api.h` adds. Diluvium's
changes to the Lua sources are held to 14 files and checked against
pristine upstream on every build, which is what keeps rebasing onto new
Lua releases tractable.

## Changelog

[`CHANGELOG.md`](CHANGELOG.md) records what changed in each release. It is
generated from [`CHANGELOG.yaml`](CHANGELOG.yaml), which is the source of
truth for the release page and the release mirror alike -- edit that file
and run `script/changelog.py generate`.

## Testing

The suite lives in `test/`. `test/run_tests.sh` is the single source of truth
for which tests run, which are skipped and why — add new tests there, not to
the Makefile.

``` sh
make test_cases                        # build the debug binary, run everything
make test_one T="strings test_fstrings"  # run named tests
make failing_test_cases                # show what is skipped, and why
./test/run_tests.sh --include-skipped   # run the skipped ones anyway
```

The runner keeps going after a failure and prints a summary, so one broken
test does not hide the state of the rest. It exits non-zero if anything failed.

## Continuous integration

Three workflows, all runnable by hand against **any commit** — enter a SHA in
the `ref` box and every job builds that exact tree.

| Workflow | Fires on | Does |
| :--- | :--- | :--- |
| **Tests** | every push and PR, or manually | Test suite on Linux and macOS, analysis-report validation, obfuscation audit |
| **Build** | manually, or called by Release | Every platform artifact plus `SHA256SUMS.txt` and `BUILDINFO.txt`, uploaded to the run |
| **Release** | a `v*` tag push, or manually | Tests, then a full build, then optionally publishes |

**Releases are opt-in.** A manual Release run defaults to `publish: false`, so
it is a full rehearsal — tests, every platform, checksums — that leaves the
Releases page untouched. Flip `publish` on when you actually want it out; it
creates the tag at the commit you named, so you can cut from any commit
without moving a branch first. Pushing a `v*` tag publishes as before.

Artifact names carry the short commit SHA, and `BUILDINFO.txt` records the
commit, build time and workflow run, so a downloaded binary can always be
traced back to its source.
