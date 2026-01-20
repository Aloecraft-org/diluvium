# Diluvium

<div align="center">

<img src="doc/icon.png" style="height:96px; width:96px;"/>

**Lua for Modern Development**

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Lua Version](https://img.shields.io/badge/lua-5.4.7-purple.svg)](https://www.lua.org/)
[![WebAssembly](https://img.shields.io/badge/WebAssembly-Ready-orange.svg)](https://webassembly.org/)

[Try it Online](https://diluvium.aloecraft.org) | [Documentation](https://www.lua.org/docs.html)

</div>

## What is Diluvium?

The Diluvium Programming Language is a **100% backward-compatible** extension of Lua with modern features like string interpolation, null coalescing, security enhancements, and more, without breaking legacy code.

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

-- And null coalescing
local config = user_config ?? default_config

-- And coming soon: switch/match, defer, compound assignment, and more
```

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