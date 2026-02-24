# Diluvium Bytecode Analysis Report

## Overview

The Diluvium compiler (`luac`) can emit a static analysis report for any Lua 5.4 source file. The report is a JSON document structured for direct deserialization into a protobuf message. It describes every function in the compiled chunk — including nested and anonymous closures — along with the globals they define, the constants they use, and the call sites they contain.

---

## Usage

### Compiler flag

```sh
diluvium_compiler -r <source.lua>
```

Compiles `source.lua` and writes the analysis report to `luac.out` in the current directory. The normal bytecode output is suppressed when `-r` is used.

### C API

```c
#include "analyze.h"

InterfaceReport *analyze_proto(const Proto *f);
void             print_report_json(InterfaceReport *report, FILE *out);
char            *report_to_json_string(InterfaceReport *report);
void             free_report(InterfaceReport *report);
```

`analyze_proto` accepts the top-level `Proto` produced by the Lua compiler and returns a heap-allocated `InterfaceReport`. Pass the result to `print_report_json` to write JSON to any `FILE *`, or to `report_to_json_string` to get a null-terminated heap string (caller must `free()`). Always call `free_report` when done.

---

## Output Schema

All fields are always present. Arrays are never absent. Booleans are `true`/`false`. Enum fields are emitted as integers matching the proto definitions below.

### Top level

```json
{
  "lua_version": "5.4.7_rc4",
  "functions": [ ...FunctionInfo ],
  "globals":   [ ...GlobalEntry  ]
}
```

---

### `FunctionInfo`

One entry per `Proto`, including all nested closures. The array is ordered depth-first: the chunk is always index 0, and `child_proto_indices` gives the indices of each function's direct children.

| Field | Type | Notes |
|---|---|---|
| `source` | string | Source filename, prefixed with `@` for files |
| `line_defined` | int | First line; `0` for the chunk |
| `last_line` | int | Last line |
| `param_count` | int | Number of fixed parameters |
| `is_vararg` | bool | Declared as vararg |
| `is_vararg_used` | bool | `OP_VARARG` actually appears in bytecode |
| `is_method` | bool | First parameter is named `self` |
| `param_names` | string[] | Parameter names in declaration order |
| `upvalue_names` | string[] | Captured upvalue names (`_ENV` for top-level) |
| `return_kind` | int | See `ReturnKind` enum |
| `table_info` | TableInfo | Populated when `return_kind == 2` (TABLE) |
| `closures` | ClosureInfo[] | Closures defined in this function that capture upvalues |
| `constants` | ConstantEntry[] | Full constant pool for this proto |
| `child_proto_indices` | int[] | Indices into `functions[]` of direct nested protos |
| `call_sites` | CallSite[] | Every `OP_CALL` / `OP_TAILCALL` in this function |
| `reads` | ReadEntry[] | Deduplicated `_ENV` and field reads |

---

### `ReturnKind`

```protobuf
enum ReturnKind {
  RETURN_KIND_UNKNOWN  = 0;  // could not determine statically
  RETURN_KIND_VOID     = 1;  // no return value
  RETURN_KIND_TABLE    = 2;  // returns a freshly constructed table
  RETURN_KIND_CALL     = 3;  // forwards the result of another call
  RETURN_KIND_UPVALUE  = 4;  // returns a value read from _ENV or a field
  RETURN_KIND_CONSTANT = 5;  // returns a literal constant
  RETURN_KIND_MULTI    = 6;  // vararg or multiple return values
  RETURN_KIND_MIXED    = 7;  // multiple return sites with different kinds
}
```

**Reliability note:** `RETURN_KIND_TABLE` is fully reliable when the table is constructed inline at the return site (e.g. `return { field = value }`). When a function returns a variable that was assigned a table earlier in its body, the analyzer may report `RETURN_KIND_UPVALUE` instead, because the table constructor and the return instruction are not adjacent in the register trace.

---

### `TableInfo`

Populated only when `return_kind == RETURN_KIND_TABLE`.

| Field | Type | Notes |
|---|---|---|
| `array_size` | int | Preallocated array slots |
| `hash_size` | int | Preallocated hash slots |
| `estimated_bytes` | int | `32 + array_size*16 + hash_size*32` |
| `contains_closures` | bool | Any `OP_CLOSURE` was seen in this function |

---

### `ConstantEntry`

One entry per slot in the proto's constant pool (`f->k`).

| Field | Type | Notes |
|---|---|---|
| `kind` | int | See `ConstantKind` enum |
| `s_val` | string\|null | String value; non-null only when `kind == 0` |
| `i_val` | int64 | Integer value; valid when `kind == 1` |
| `f_val` | double | Float value; valid when `kind == 2` |
| `b_val` | bool | Boolean value; valid when `kind == 3` |

```protobuf
enum ConstantKind {
  CONST_KIND_STRING  = 0;
  CONST_KIND_INTEGER = 1;
  CONST_KIND_FLOAT   = 2;
  CONST_KIND_BOOL    = 3;
  CONST_KIND_NULL    = 4;
}
```

**Reliability note:** String constants are complete and reliable — every string literal in the source appears in the pool of its enclosing proto. Integer constants are best-effort: small integers that fit in the `sBx` field of `OP_LOADI` are encoded as instruction immediates and will not appear in the pool. The threshold is approximately ±65535. Floats and booleans are reliable.

---

### `ClosureInfo`

One entry per `OP_CLOSURE` in this function that captures at least one upvalue.

| Field | Type | Notes |
|---|---|---|
| `line_defined` | int | `linedefined` of the child proto |
| `upvalue_count` | int | Number of upvalues captured |

---

### `CallSite`

One entry per `OP_CALL` or `OP_TAILCALL`.

| Field | Type | Notes |
|---|---|---|
| `line` | int | Source line, or `0` if debug info was stripped |
| `kind` | int | See `CallKind` enum |
| `callee` | string | Resolved callee name, or `""` if unresolvable |
| `arg_count` | int | Number of arguments; `-1` means variable |
| `is_tail` | bool | `true` for `OP_TAILCALL` |

```protobuf
enum CallKind {
  CALL_KIND_UNKNOWN = 0;  // could not resolve
  CALL_KIND_GLOBAL  = 1;  // _ENV.name  (direct global call)
  CALL_KIND_FIELD   = 2;  // table.method  (one-level field call)
  CALL_KIND_METHOD  = 3;  // obj:method  (OP_SELF)
  CALL_KIND_LOCAL   = 4;  // local variable or register
}
```

**Resolution depth:** `CALL_KIND_GLOBAL` and `CALL_KIND_FIELD` are resolved one level deep. For `CALL_KIND_FIELD`, the `callee` string is `"table.method"` where `table` is the name of the global or upvalue that was loaded into the base register. Chains deeper than one level (e.g. `a.b.c()`) are reported as `CALL_KIND_UNKNOWN`.

---

### `ReadEntry`

One deduplicated entry per distinct `(table_name, field_name)` pair read by `OP_GETTABUP` or `OP_GETFIELD` in this function.

| Field | Type | Notes |
|---|---|---|
| `table_name` | string | `"_ENV"` for globals; upvalue name otherwise; `"?"` for unresolved register sources |
| `field_name` | string | The string key being read |

---

### `GlobalEntry`

One entry per name written to `_ENV` (upvalue 0) anywhere in the chunk.

| Field | Type | Notes |
|---|---|---|
| `name` | string | The global name |
| `is_function` | bool | `true` if a closure was directly assigned to this name |
| `function_index` | int | Index into `functions[]` of the assigned closure; `-1` if not resolvable or not a function |

**Conditional globals:** Globals defined inside a function body (rather than at chunk scope) only exist at runtime after the enclosing function has been called. The chunk proto has `line_defined == 0`; any global whose defining `SETTABUP` occurs in a proto with `line_defined > 0` is conditionally defined. The analyzer does not currently flag this distinction explicitly — consumers should cross-reference `function_index` against `functions[0].child_proto_indices` to determine whether a global is unconditionally registered at load time.

---

## Caveats

- **Stripped bytecode:** Line numbers in `CallSite.line` will be `0` if the chunk was compiled without debug info.
- **Aliasing:** A global may be assigned the value of another global (e.g. `some_obj.fn_assigned = do_something`). The report records the assignment target but does not currently emit an alias map. The `reads` array of the assigning function provides indirect evidence of the source.
- **Computed keys:** `SETTABUP` with a non-constant key (runtime string) is not captured in the globals list.
- **Multiple assignment:** `upsert_global` deduplicates by name. If a name is assigned multiple times, `is_function` is promoted to `true` if any assignment is a closure, but `function_index` reflects only the first resolvable closure assignment.