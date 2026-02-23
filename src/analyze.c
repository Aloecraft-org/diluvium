/*
** analyze.c
** Lua 5.4 bytecode analyzer for luac --report flag
**
** JSON output is structured for direct deserialization into a protobuf message.
** All fields are always present (no optional omissions), arrays are always
** arrays (never absent), and field names are snake_case throughout.
**
** Target: Lua 5.4.7_rc4 (Diluvium fork)
*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lanalyze_c
#define LUA_CORE

#include "lua.h"
#include "lobject.h"
#include "lstate.h"
#include "lundump.h"
#include "lopcodes.h"

/* -------------------------------------------------------------------------
** Version tag burned in at compile time.
** Change this if the fork version string changes.
** ------------------------------------------------------------------------- */
#define DILUVIUM_LUA_VERSION "5.4.7_rc4"


/* -------------------------------------------------------------------------
** Internal data structures
** ------------------------------------------------------------------------- */

/*
** ReturnKind classifies what a return site hands back.
** Maps directly to a proto enum:
**
**   enum ReturnKind {
**     RETURN_KIND_UNKNOWN  = 0;
**     RETURN_KIND_VOID     = 1;
**     RETURN_KIND_TABLE    = 2;
**     RETURN_KIND_CALL     = 3;   // result of a function call
**     RETURN_KIND_UPVALUE  = 4;
**     RETURN_KIND_CONSTANT = 5;
**     RETURN_KIND_MULTI    = 6;   // multiple values / vararg
**   }
*/
typedef enum {
  RETURN_KIND_UNKNOWN  = 0,
  RETURN_KIND_VOID     = 1,
  RETURN_KIND_TABLE    = 2,
  RETURN_KIND_CALL     = 3,
  RETURN_KIND_UPVALUE  = 4,
  RETURN_KIND_CONSTANT = 5,
  RETURN_KIND_MULTI    = 6
} ReturnKind;

/*
** TableInfo — populated only when return_kind == RETURN_KIND_TABLE.
** Maps to a proto message:
**
**   message TableInfo {
**     int32  array_size       = 1;
**     int32  hash_size        = 2;
**     int64  estimated_bytes  = 3;
**     bool   contains_closures = 4;
**   }
*/
typedef struct {
  int    array_size;
  int    hash_size;
  size_t estimated_bytes;
  int    contains_closures;   /* bool */
} TableInfo;

/*
** ClosureInfo — one entry per OP_CLOSURE that captures upvalues.
** Maps to a proto message:
**
**   message ClosureInfo {
**     int32 line_defined = 1;
**     int32 upvalue_count = 2;
**   }
*/
typedef struct {
  int line_defined;
  int upvalue_count;
} ClosureInfo;

/*
** FunctionInfo — one entry per Proto, including nested ones.
** Maps to a proto message:
**
**   message FunctionInfo {
**     string         source          = 1;
**     int32          line_defined    = 2;
**     int32          last_line       = 3;
**     int32          param_count     = 4;
**     bool           is_vararg       = 5;
**     bool           is_method       = 6;
**     repeated string param_names    = 7;
**     repeated string upvalue_names  = 8;
**     ReturnKind     return_kind     = 9;
**     TableInfo      table_info      = 10;  // present iff return_kind==TABLE
**     repeated ClosureInfo closures  = 11;
**   }
*/
typedef struct {
  /* identity */
  const char *source;
  int         line_defined;
  int         last_line;

  /* signature */
  int         param_count;
  int         is_vararg;
  int         is_method;          /* first param is "self" */
  const char **param_names;       /* [param_count] */
  int         upvalue_count;
  const char **upvalue_names;     /* [upvalue_count] */

  /* return analysis */
  ReturnKind  return_kind;
  TableInfo   table_info;         /* valid iff return_kind == RETURN_KIND_TABLE */

  /* closure tracking */
  ClosureInfo *closures;
  int          num_closures;
  int          cap_closures;
} FunctionInfo;

/*
** GlobalEntry — a name set in _ENV at top level.
** Maps to a proto message:
**
**   message GlobalEntry {
**     string name        = 1;
**     bool   is_function = 2;
**   }
*/
typedef struct {
  const char *name;
  int         is_function;   /* bool */
} GlobalEntry;

/*
** InterfaceReport — the top-level output message.
**
**   message InterfaceReport {
**     string               lua_version  = 1;
**     repeated FunctionInfo functions   = 2;
**     repeated GlobalEntry  globals     = 3;
**   }
*/
typedef struct {
  FunctionInfo *functions;
  int           num_functions;
  int           cap_functions;

  GlobalEntry  *globals;
  int           num_globals;
  int           cap_globals;
} InterfaceReport;


/* -------------------------------------------------------------------------
** Small utilities
** ------------------------------------------------------------------------- */

static const char *str_dup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *copy = (char *)malloc(n + 1);
  memcpy(copy, s, n + 1);
  return copy;
}

/* Write s to out as a JSON string, escaping as required by RFC 8259. */
static void json_write_string(FILE *out, const char *s) {
  fputc('"', out);
  if (!s) { fputc('"', out); return; }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"':  fputs("\\\"", out); break;
      case '\\': fputs("\\\\", out); break;
      case '\b': fputs("\\b",  out); break;
      case '\f': fputs("\\f",  out); break;
      case '\n': fputs("\\n",  out); break;
      case '\r': fputs("\\r",  out); break;
      case '\t': fputs("\\t",  out); break;
      default:
        if (*p < 0x20)
          fprintf(out, "\\u%04x", *p);
        else
          fputc(*p, out);
        break;
    }
  }
  fputc('"', out);
}


/* -------------------------------------------------------------------------
** OP_NEWTABLE size decoding for Lua 5.4
**
** From lopcodes.h notes:
**   B  = log2(hash_size) + 1, or 0 for empty hash part
**   C  = array_size  (or low bits if k flag set, high bits in EXTRAARG)
**
** The old fb2int floating-point-byte encoding was a Lua 5.1/5.2 thing and
** does NOT apply here.
** ------------------------------------------------------------------------- */

static int decode_hash_size(int b) {
  /* B == 0 → 0 slots; otherwise 1 << (B-1) */
  return (b == 0) ? 0 : (1 << (b - 1));
}

/*
** Peek at the next instruction to resolve the EXTRAARG for array size.
** pc is the index of OP_NEWTABLE, so pc+1 is always OP_EXTRAARG per spec.
*/
static int decode_array_size(const Proto *f, int pc) {
  Instruction newtable = f->code[pc];
  int k   = GETARG_k(newtable);
  int c   = GETARG_C(newtable);

  if (!k) return c;

  /* k=1: real array size = EXTRAARG:C (high bits from EXTRAARG, low 8 from C) */
  if (pc + 1 < f->sizecode) {
    Instruction extra = f->code[pc + 1];
    if (GET_OPCODE(extra) == OP_EXTRAARG)
      return (GETARG_Ax(extra) << 8) | c;
  }
  return c; /* fallback — shouldn't happen in well-formed bytecode */
}


/* -------------------------------------------------------------------------
** Report construction helpers
** ------------------------------------------------------------------------- */

static InterfaceReport *create_report(void) {
  InterfaceReport *r = (InterfaceReport *)calloc(1, sizeof(InterfaceReport));
  return r;
}

static FunctionInfo *push_function(InterfaceReport *report) {
  if (report->num_functions >= report->cap_functions) {
    int nc = report->cap_functions == 0 ? 8 : report->cap_functions * 2;
    report->functions = (FunctionInfo *)realloc(report->functions,
                                                 nc * sizeof(FunctionInfo));
    report->cap_functions = nc;
  }
  FunctionInfo *f = &report->functions[report->num_functions++];
  memset(f, 0, sizeof(FunctionInfo));
  return f;
}

static void push_closure(FunctionInfo *fi, int line, int nupvals) {
  if (fi->num_closures >= fi->cap_closures) {
    int nc = fi->cap_closures == 0 ? 4 : fi->cap_closures * 2;
    fi->closures = (ClosureInfo *)realloc(fi->closures, nc * sizeof(ClosureInfo));
    fi->cap_closures = nc;
  }
  fi->closures[fi->num_closures].line_defined  = line;
  fi->closures[fi->num_closures].upvalue_count = nupvals;
  fi->num_closures++;
}

/*
** Add a global entry, deduplicating by name.
** Later assignments win (is_function may be promoted from variable→function).
*/
static void upsert_global(InterfaceReport *report, const char *name, int is_fn) {
  /* Check for existing entry */
  for (int i = 0; i < report->num_globals; i++) {
    if (strcmp(report->globals[i].name, name) == 0) {
      /* Promote to function if we now know it is one */
      if (is_fn) report->globals[i].is_function = 1;
      return;
    }
  }
  if (report->num_globals >= report->cap_globals) {
    int nc = report->cap_globals == 0 ? 8 : report->cap_globals * 2;
    report->globals = (GlobalEntry *)realloc(report->globals,
                                              nc * sizeof(GlobalEntry));
    report->cap_globals = nc;
  }
  report->globals[report->num_globals].name        = str_dup(name);
  report->globals[report->num_globals].is_function = is_fn;
  report->num_globals++;
}


/* -------------------------------------------------------------------------
** Backward scan: given that we're at instruction `pc` and want to know
** what produced register `reg`, walk back up to `limit` instructions.
** Returns 1 if a CLOSURE wrote to reg, 0 if something else did, -1 if
** we ran out of instructions without finding a writer (indeterminate).
** ------------------------------------------------------------------------- */
static int find_reg_source(const Proto *f, int pc, int reg) {
  int limit = (pc - 16 < 0) ? 0 : pc - 16;

  for (int i = pc - 1; i >= limit; i--) {
    Instruction ins = f->code[i];
    OpCode op = GET_OPCODE(ins);
    int a = GETARG_A(ins);

    if (op == OP_CLOSURE && a == reg)
      return 1;

    /* Does this instruction write to reg? If so, it's not a closure. */
    switch (op) {
      /* All iABC / iABx / iAsBx ops that write R[A]: */
      case OP_MOVE:      case OP_LOADI:     case OP_LOADF:
      case OP_LOADK:     case OP_LOADKX:    case OP_LOADFALSE:
      case OP_LFALSESKIP: case OP_LOADTRUE: case OP_LOADNIL:
      case OP_GETUPVAL:  case OP_GETTABUP:  case OP_GETTABLE:
      case OP_GETI:      case OP_GETFIELD:  case OP_NEWTABLE:
      case OP_SELF:
      case OP_ADDI:      case OP_ADDK:      case OP_SUBK:
      case OP_MULK:      case OP_MODK:      case OP_POWK:
      case OP_DIVK:      case OP_IDIVK:     case OP_BANDK:
      case OP_BORK:      case OP_BXORK:     case OP_SHRI:
      case OP_SHLI:
      case OP_ADD:       case OP_SUB:       case OP_MUL:
      case OP_DIV:       case OP_IDIV:      case OP_MOD:
      case OP_POW:       case OP_BAND:      case OP_BOR:
      case OP_BXOR:      case OP_SHL:       case OP_SHR:
      case OP_MMBIN:     case OP_MMBINI:    case OP_MMBINK:
      case OP_UNM:       case OP_BNOT:      case OP_NOT:
      case OP_LEN:       case OP_CONCAT:
      case OP_CALL:      case OP_TAILCALL:
      case OP_VARARG:
        if (a == reg) return 0;
        break;
      default:
        break;
    }
  }
  return -1; /* indeterminate */
}


/* -------------------------------------------------------------------------
** Walk backwards from pc to find what originally produced register reg.
** Unlike find_reg_source (which stops at any writer), this one is
** specifically looking for OP_NEWTABLE and skips over instructions that
** read *from* reg without overwriting it (e.g. SETFIELD, SETI, SETLIST
** all take a table register in A but don't reassign it).
** Returns the pc of the NEWTABLE if found, -1 otherwise.
** ------------------------------------------------------------------------- */
static int find_newtable_for_reg(const Proto *f, int pc, int reg) {
  for (int i = pc - 1; i >= 0; i--) {
    Instruction ins = f->code[i];
    OpCode op = GET_OPCODE(ins);
    int a = GETARG_A(ins);

    if (op == OP_NEWTABLE && a == reg)
      return i;

    /* These ops use reg as a table to write into but do NOT reassign it —
    ** skip over them so we keep looking for the NEWTABLE. */
    if ((op == OP_SETFIELD || op == OP_SETTABLE ||
         op == OP_SETI     || op == OP_SETLIST) && a == reg)
      continue;

    /* Any other instruction that writes to reg stops the search — the
    ** register has been reassigned and the NEWTABLE is unrelated. */
    switch (op) {
      case OP_MOVE:      case OP_LOADI:     case OP_LOADF:
      case OP_LOADK:     case OP_LOADKX:    case OP_LOADFALSE:
      case OP_LFALSESKIP: case OP_LOADTRUE: case OP_LOADNIL:
      case OP_GETUPVAL:  case OP_GETTABUP:  case OP_GETTABLE:
      case OP_GETI:      case OP_GETFIELD:  case OP_SELF:
      case OP_ADDI:      case OP_ADDK:      case OP_SUBK:
      case OP_MULK:      case OP_MODK:      case OP_POWK:
      case OP_DIVK:      case OP_IDIVK:     case OP_BANDK:
      case OP_BORK:      case OP_BXORK:     case OP_SHRI:  case OP_SHLI:
      case OP_ADD:       case OP_SUB:       case OP_MUL:
      case OP_DIV:       case OP_IDIV:      case OP_MOD:
      case OP_POW:       case OP_BAND:      case OP_BOR:
      case OP_BXOR:      case OP_SHL:       case OP_SHR:
      case OP_MMBIN:     case OP_MMBINI:    case OP_MMBINK:
      case OP_UNM:       case OP_BNOT:      case OP_NOT:
      case OP_LEN:       case OP_CONCAT:
      case OP_CALL:      case OP_TAILCALL:
      case OP_CLOSURE:   case OP_VARARG:
        if (a == reg) return -1;
        break;
      default:
        break;
    }
  }
  return -1;
}

/* -------------------------------------------------------------------------
** Classify what a return instruction returns.
** ------------------------------------------------------------------------- */
static ReturnKind classify_return(const Proto *f, int pc,
                                  int last_newtable_pc) {
  Instruction ins = f->code[pc];
  OpCode op = GET_OPCODE(ins);

  if (op == OP_RETURN0)
    return RETURN_KIND_VOID;

  if (op == OP_RETURN1) {
    int reg = GETARG_A(ins);

    /* Walk back looking for the NEWTABLE that produced this register,
    ** skipping over SETFIELD/SETI/SETTABLE/SETLIST which mutate the table
    ** without reassigning the register. This correctly handles patterns like:
    **   NEWTABLE  r
    **   SETFIELD  r, "host", ...
    **   SETFIELD  r, "port", ...
    **   RETURN1   r
    */
    if (find_newtable_for_reg(f, pc, reg) >= 0)
      return RETURN_KIND_TABLE;

    /* Walk back for other value sources */
    int limit = (pc - 24 < 0) ? 0 : pc - 24;
    for (int i = pc - 1; i >= limit; i--) {
      Instruction prev = f->code[i];
      OpCode pop = GET_OPCODE(prev);
      if (GETARG_A(prev) != reg) continue;
      if (pop == OP_CALL || pop == OP_TAILCALL) return RETURN_KIND_CALL;
      if (pop == OP_GETUPVAL)                   return RETURN_KIND_UPVALUE;
      if (pop == OP_CLOSURE)                    return RETURN_KIND_UNKNOWN;
      if (pop == OP_LOADK   || pop == OP_LOADI  ||
          pop == OP_LOADF   || pop == OP_LOADTRUE ||
          pop == OP_LOADFALSE)                  return RETURN_KIND_CONSTANT;
      break;
    }
    return RETURN_KIND_UNKNOWN;
  }

  /* OP_RETURN: A=first reg, B=count+1 (0 = variable) */
  {
    int b = GETARG_B(ins);
    if (b == 1) return RETURN_KIND_VOID;   /* return with 0 values */
    if (b == 0) return RETURN_KIND_MULTI;  /* variable return count */

    if (b == 2) {
      /* Single value — same register-tracing logic as RETURN1 */
      int reg = GETARG_A(ins);
      if (find_newtable_for_reg(f, pc, reg) >= 0)
        return RETURN_KIND_TABLE;
    }
    if (b > 2) return RETURN_KIND_MULTI;

    return RETURN_KIND_UNKNOWN;
  }
}


/* -------------------------------------------------------------------------
** Core analysis pass over a single Proto
** ------------------------------------------------------------------------- */
static void analyze_proto_recursive(const Proto *f, InterfaceReport *report);

static void analyze_function(const Proto *f, InterfaceReport *report) {
  FunctionInfo *fi = push_function(report);

  /* --- Identity --------------------------------------------------------- */
  fi->source       = f->source ? str_dup(getstr(f->source)) : str_dup("?");
  fi->line_defined = f->linedefined;
  fi->last_line    = f->lastlinedefined;

  /* --- Signature -------------------------------------------------------- */
  fi->param_count  = f->numparams;
  fi->is_vararg    = f->is_vararg ? 1 : 0;

  if (f->numparams > 0) {
    fi->param_names = (const char **)malloc(f->numparams * sizeof(const char *));
    for (int i = 0; i < f->numparams; i++) {
      const char *pname = "(?)";
      if (i < f->sizelocvars && f->locvars[i].varname)
        pname = getstr(f->locvars[i].varname);
      fi->param_names[i] = str_dup(pname);
    }
    /* Detect method: first param named "self" */
    fi->is_method = (strcmp(fi->param_names[0], "self") == 0) ? 1 : 0;
  }

  fi->upvalue_count = f->sizeupvalues;
  if (f->sizeupvalues > 0) {
    fi->upvalue_names = (const char **)malloc(f->sizeupvalues * sizeof(const char *));
    for (int i = 0; i < f->sizeupvalues; i++) {
      const char *uname = "(?)";
      if (f->upvalues && f->upvalues[i].name)
        uname = getstr(f->upvalues[i].name);
      fi->upvalue_names[i] = str_dup(uname);
    }
  }

  /* --- Bytecode scan ---------------------------------------------------- */
  int last_newtable_pc  = -1;
  int last_newtable_arr = 0;
  int last_newtable_hsh = 0;
  fi->return_kind = RETURN_KIND_UNKNOWN;

  for (int pc = 0; pc < f->sizecode; pc++) {
    Instruction ins = f->code[pc];
    OpCode op = GET_OPCODE(ins);

    switch (op) {

      case OP_NEWTABLE: {
        last_newtable_pc  = pc;
        last_newtable_arr = decode_array_size(f, pc);
        last_newtable_hsh = decode_hash_size(GETARG_B(ins));
        break;
      }

      case OP_RETURN:
      case OP_RETURN0:
      case OP_RETURN1: {
        ReturnKind kind = classify_return(f, pc, last_newtable_pc);
        /* Priority: TABLE beats everything; VOID (compiler guard RETURN0)
        ** must not clobber a TABLE or CALL already recorded. */
        int should_update = 0;
        if (fi->return_kind == RETURN_KIND_UNKNOWN)
          should_update = 1;
        else if (kind == RETURN_KIND_TABLE)
          should_update = 1;
        else if (kind != RETURN_KIND_UNKNOWN && kind != RETURN_KIND_VOID
                 && fi->return_kind == RETURN_KIND_VOID)
          should_update = 1;
        if (should_update)
          fi->return_kind = kind;

        if (kind == RETURN_KIND_TABLE) {
          /* Use the specific NEWTABLE for the returned register so sizes
          ** are correct even when multiple tables exist in the function. */
          int ret_reg = GETARG_A(ins);
          int nt_pc   = find_newtable_for_reg(f, pc, ret_reg);
          if (nt_pc >= 0) {
            Instruction nt = f->code[nt_pc];
            fi->table_info.array_size = decode_array_size(f, nt_pc);
            fi->table_info.hash_size  = decode_hash_size(GETARG_B(nt));
          } else {
            fi->table_info.array_size = last_newtable_arr;
            fi->table_info.hash_size  = last_newtable_hsh;
          }
          fi->table_info.estimated_bytes = 32
            + (size_t)(fi->table_info.array_size * 16)
            + (size_t)(fi->table_info.hash_size  * 32);
        }
        break;
      }

      case OP_CLOSURE: {
        int bx = GETARG_Bx(ins);
        if (bx < f->sizep) {
          Proto *child = f->p[bx];
          if (child->sizeupvalues > 0) {
            push_closure(fi, child->linedefined, child->sizeupvalues);
            /* If we end up returning a table, flag that it contains closures */
            fi->table_info.contains_closures = 1;
          }
        }
        break;
      }

      case OP_SETTABUP: {
        /* UpValue[A][K[B]] := RK(C)
        ** We only care about _ENV (upvalue 0) assignments at the top level. */
        if (GETARG_A(ins) != 0) break;

        int key_idx = GETARG_B(ins);
        int val_reg = GETARG_C(ins);

        if (key_idx >= f->sizek) break;
        TValue *k = &f->k[key_idx];
        if (!ttisstring(k)) break;

        const char *name = getstr(tsvalue(k));
        int is_fn = (find_reg_source(f, pc, val_reg) == 1) ? 1 : 0;
        upsert_global(report, name, is_fn);
        break;
      }

      /* OP_2Q (Diluvium null-coalescing): writes R[A], treat like a normal
      ** register-producing instruction. No special analysis needed here. */
      case OP_2Q:
        break;

      default:
        break;
    }
  }

  /* If return_kind is still UNKNOWN and the function has no code that
  ** produces a value, call it VOID. */
  if (fi->return_kind == RETURN_KIND_UNKNOWN && f->sizecode > 0) {
    /* Leave as UNKNOWN — the caller may have multiple paths. */
  }

  /* --- Recurse into nested protos --------------------------------------- */
  for (int i = 0; i < f->sizep; i++)
    analyze_proto_recursive(f->p[i], report);
}

static void analyze_proto_recursive(const Proto *f, InterfaceReport *report) {
  analyze_function(f, report);
}


/* -------------------------------------------------------------------------
** Public entry point
** ------------------------------------------------------------------------- */
InterfaceReport *analyze_proto(const Proto *f) {
  InterfaceReport *report = create_report();
  analyze_proto_recursive(f, report);
  return report;
}


/* -------------------------------------------------------------------------
** JSON serialization
**
** Schema mirrors the proto definition at the top of this file.
** Every field is always emitted. Arrays are always arrays. Booleans are
** always true/false (never 0/1). Enums are emitted as their integer value
** so proto can decode them directly.
** ------------------------------------------------------------------------- */

static void write_indent(FILE *out, int depth) {
  for (int i = 0; i < depth * 2; i++) fputc(' ', out);
}

static void write_string_array(FILE *out, const char **arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    write_indent(out, depth + 1);
    json_write_string(out, arr[i]);
    if (i < n - 1) fputc(',', out);
    fputc('\n', out);
  }
  write_indent(out, depth);
  fputc(']', out);
}

static void write_table_info(FILE *out, const TableInfo *ti, int depth) {
  fprintf(out, "{\n");
  write_indent(out, depth + 1);
  fprintf(out, "\"array_size\": %d,\n", ti->array_size);
  write_indent(out, depth + 1);
  fprintf(out, "\"hash_size\": %d,\n", ti->hash_size);
  write_indent(out, depth + 1);
  fprintf(out, "\"estimated_bytes\": %zu,\n", ti->estimated_bytes);
  write_indent(out, depth + 1);
  fprintf(out, "\"contains_closures\": %s\n", ti->contains_closures ? "true" : "false");
  write_indent(out, depth);
  fputc('}', out);
}

static void write_closure_array(FILE *out, const ClosureInfo *arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    write_indent(out, depth + 1);
    fprintf(out, "{\"line_defined\": %d, \"upvalue_count\": %d}",
            arr[i].line_defined, arr[i].upvalue_count);
    if (i < n - 1) fputc(',', out);
    fputc('\n', out);
  }
  write_indent(out, depth);
  fputc(']', out);
}

static void write_function_info(FILE *out, const FunctionInfo *fi, int depth) {
  write_indent(out, depth); fprintf(out, "{\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"source\": "); json_write_string(out, fi->source); fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"line_defined\": %d,\n", fi->line_defined);

  write_indent(out, depth + 1);
  fprintf(out, "\"last_line\": %d,\n", fi->last_line);

  write_indent(out, depth + 1);
  fprintf(out, "\"param_count\": %d,\n", fi->param_count);

  write_indent(out, depth + 1);
  fprintf(out, "\"is_vararg\": %s,\n", fi->is_vararg ? "true" : "false");

  write_indent(out, depth + 1);
  fprintf(out, "\"is_method\": %s,\n", fi->is_method ? "true" : "false");

  write_indent(out, depth + 1);
  fprintf(out, "\"param_names\": ");
  if (fi->param_count > 0 && fi->param_names)
    write_string_array(out, fi->param_names, fi->param_count, depth + 1);
  else
    fprintf(out, "[]");
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"upvalue_names\": ");
  if (fi->upvalue_count > 0 && fi->upvalue_names)
    write_string_array(out, fi->upvalue_names, fi->upvalue_count, depth + 1);
  else
    fprintf(out, "[]");
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"return_kind\": %d,\n", (int)fi->return_kind);

  write_indent(out, depth + 1);
  fprintf(out, "\"table_info\": ");
  write_table_info(out, &fi->table_info, depth + 1);
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"closures\": ");
  write_closure_array(out, fi->closures, fi->num_closures, depth + 1);
  fprintf(out, "\n");

  write_indent(out, depth); fputc('}', out);
}

void print_report_json(InterfaceReport *report, FILE *out) {
  fprintf(out, "{\n");

  /* Version tag */
  fprintf(out, "  \"lua_version\": \"%s\",\n", DILUVIUM_LUA_VERSION);

  /* Functions array */
  fprintf(out, "  \"functions\": [\n");
  for (int i = 0; i < report->num_functions; i++) {
    write_function_info(out, &report->functions[i], 2);
    if (i < report->num_functions - 1) fputc(',', out);
    fputc('\n', out);
  }
  fprintf(out, "  ],\n");

  /* Globals array */
  fprintf(out, "  \"globals\": [\n");
  for (int i = 0; i < report->num_globals; i++) {
    fprintf(out, "    {\"name\": ");
    json_write_string(out, report->globals[i].name);
    fprintf(out, ", \"is_function\": %s}",
            report->globals[i].is_function ? "true" : "false");
    if (i < report->num_globals - 1) fputc(',', out);
    fputc('\n', out);
  }
  fprintf(out, "  ]\n");

  fprintf(out, "}\n");
}

char *report_to_json_string(InterfaceReport *report) {
  char  *buf    = NULL;
  size_t size   = 0;
  FILE  *stream = open_memstream(&buf, &size);
  if (!stream) return NULL;
  print_report_json(report, stream);
  fclose(stream);
  return buf; /* caller must free() */
}


/* -------------------------------------------------------------------------
** Memory cleanup
** ------------------------------------------------------------------------- */
void free_report(InterfaceReport *report) {
  if (!report) return;

  for (int i = 0; i < report->num_functions; i++) {
    FunctionInfo *fi = &report->functions[i];
    free((void *)fi->source);
    for (int j = 0; j < fi->param_count; j++)
      free((void *)fi->param_names[j]);
    free(fi->param_names);
    for (int j = 0; j < fi->upvalue_count; j++)
      free((void *)fi->upvalue_names[j]);
    free(fi->upvalue_names);
    free(fi->closures);
  }
  free(report->functions);

  for (int i = 0; i < report->num_globals; i++)
    free((void *)report->globals[i].name);
  free(report->globals);

  free(report);
}