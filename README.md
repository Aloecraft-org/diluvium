# Diluvium

<div align="center">

<img src="doc/icon.png" style="height:96px; width:96px;"/>

**Lua for Modern Development**

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Repo](https://img.shields.io/badge/github-repo-blue.svg)](https://github.com/Aloecraft-org/diluvium)
[![Lua Version](https://img.shields.io/badge/lua-5.4.7-purple.svg)](https://www.lua.org/)
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

**And coming soon:** 
- switch/match
- defer/with
- compound assignment
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
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_linux_static_x86_64
cp diluvium_linux_static_x86_64 diluvium && chmod +x diluvium
./diluvium
```

**Linux (aarch64/Raspberry Pi 3/4/5) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_linux_static_aarch64
cp diluvium_linux_static_aarch64 diluvium && chmod +x diluvium
./diluvium
```

**Linux (32bit armv7l/Raspberry Pi 1/2/Zero/Zero W) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_linux_static_armv7l
cp diluvium_linux_static_armv7l diluvium && chmod +x diluvium
./diluvium
```

**MacOS (ARM64) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_darwin_arm64
cp diluvium_darwin_arm64 diluvium && chmod +x diluvium
./diluvium
```

**MacOS (Intel) Installation**
``` sh
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_darwin_x86_64
cp diluvium_darwin_x86_64 diluvium && chmod +x diluvium
./diluvium
```

**Windows Installation**
``` ps
wget https://github.com/Aloecraft-org/diluvium/releases/download/v5.4.7_rc2/diluvium_windows_x86_64.exe -OutFile diluvium.exe
./diluvium
```

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