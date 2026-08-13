# Diluvium for Visual Studio Code

Syntax highlighting and editing support for the
[Diluvium](https://github.com/Aloecraft-org/diluvium) programming language —
Lua with string interpolation, null coalescing, `switch`, `defer`, `with`,
safe navigation, compound assignment and secure functions.

Files with the `.dlua` extension (and `*.host.lua` host configurations) are
recognised automatically. Because `.dlua` is its own language ID, generic Lua
language servers no longer attach to these files, so Diluvium syntax such as
`$"hello, {name}"` and `a ?? b` is not flagged as an error.

## What is highlighted

- Everything standard Lua has: keywords, strings, long strings, comments,
  numbers (including hex floats), labels, `goto`
- Interpolated strings `$"..."` / `$'...'` with full highlighting of the
  embedded expressions, `\{` `\}` escapes, and `::%fmt` format specifications
- `??` and `??=` null coalescing, `?.` and `?[` safe navigation
- Compound assignment: `+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `^=`, `..=`,
  `&=`, `|=`, `<<=`, `>>=`, `??=`
- Contextual keywords: `switch` / `case` / `default`, `defer`, `with`
- The `global` keyword and `~function` secure functions
- The Diluvium standard libraries: `queue`, `json`, `msgpack`, `endpoint`,
  `bytes`, `time` alongside the stock Lua ones

Bracket matching, auto-closing pairs, `--` line and `--[[ ]]` block comment
toggling, and indentation rules for `then`/`do`/`end` blocks (including
`switch`/`case` and `defer do`) are included.

## Installing from the repository

The extension is plain JSON — no build step. Either copy the folder:

```sh
cp -r editors/vscode ~/.vscode/extensions/aloecraft.diluvium-0.1.0
```

or package a `.vsix` with [`vsce`](https://github.com/microsoft/vscode-vsce):

```sh
cd editors/vscode
npx @vscode/vsce package
code --install-extension diluvium-0.1.0.vsix
```

Then reload VS Code and open any `.dlua` file.

## Using `.lua` files

If you keep Diluvium code in plain `.lua` files, you can map them to this
language per-workspace in `.vscode/settings.json`:

```json
{
  "files.associations": {
    "*.lua": "dlua"
  }
}
```

## Not included (yet)

This extension provides grammar-level support only. There is no language
server, so no completion, go-to-definition, or semantic diagnostics — but
also no false syntax errors on Diluvium constructs.
