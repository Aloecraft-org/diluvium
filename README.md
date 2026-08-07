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

Check back frequently for updates

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

## Embedding a REPL

`doc/repl-reference.html` is a working Diluvium REPL in a browser
terminal, and the reference for driving the WASM build: load
`libdiluvium_wasi.wasm`, call `__wasm_call_ctors`, then `repl_eval` for
evaluation (it reports whether input is merely unfinished) and
`repl_complete` for completion.

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
