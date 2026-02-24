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
/*
** ConstantKind classifies entries in the constant pool.
** Maps directly to a proto enum:
**
**   enum ConstantKind {
**     CONST_KIND_STRING  = 0;
**     CONST_KIND_INTEGER = 1;
**     CONST_KIND_FLOAT   = 2;
**     CONST_KIND_BOOL    = 3;
**     CONST_KIND_NULL    = 4;
**   }
*/
typedef enum {
  CONST_KIND_STRING  = 0,
  CONST_KIND_INTEGER = 1,
  CONST_KIND_FLOAT   = 2,
  CONST_KIND_BOOL    = 3,
  CONST_KIND_NULL    = 4
} ConstantKind;

/*
** ConstantEntry — one entry per slot in f->k.
** Maps to a proto message:
**
**   message ConstantEntry {
**     ConstantKind kind    = 1;
**     string       s_val   = 2;  // CONST_KIND_STRING
**     int64        i_val   = 3;  // CONST_KIND_INTEGER
**     double       f_val   = 4;  // CONST_KIND_FLOAT
**     bool         b_val   = 5;  // CONST_KIND_BOOL
**   }
*/
typedef struct {
  ConstantKind kind;
  const char  *s_val;   /* owned; non-NULL iff kind == CONST_KIND_STRING */
  lua_Integer  i_val;
  lua_Number   f_val;
  int          b_val;   /* bool */
} ConstantEntry;

typedef enum {
  RETURN_KIND_UNKNOWN  = 0,
  RETURN_KIND_VOID     = 1,
  RETURN_KIND_TABLE    = 2,
  RETURN_KIND_CALL     = 3,
  RETURN_KIND_UPVALUE  = 4,
  RETURN_KIND_CONSTANT = 5,
  RETURN_KIND_MULTI    = 6,
  RETURN_KIND_MIXED    = 7   /* multiple return sites with different kinds */
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
** CallKind classifies how a call site was resolved.
**
**   enum CallKind {
**     CALL_KIND_UNKNOWN  = 0;  // could not resolve callee name
**     CALL_KIND_GLOBAL   = 1;  // _ENV.name  (GETTABUP upvalue 0)
**     CALL_KIND_FIELD    = 2;  // table.method (GETFIELD, one level)
**     CALL_KIND_METHOD   = 3;  // obj:method   (SELF)
**     CALL_KIND_LOCAL    = 4;  // local variable / register
**   }
*/
typedef enum {
  CALL_KIND_UNKNOWN = 0,
  CALL_KIND_GLOBAL  = 1,
  CALL_KIND_FIELD   = 2,
  CALL_KIND_METHOD  = 3,
  CALL_KIND_LOCAL   = 4
} CallKind;

/*
** CallSite — one entry per OP_CALL or OP_TAILCALL.
** Maps to a proto message:
**
**   message CallSite {
**     int32    line        = 1;
**     CallKind kind        = 2;
**     string   callee      = 3;  // e.g. "print", "ego.emit", "obj:method"
**     int32    arg_count   = 4;  // -1 = variable
**     bool     is_tail     = 5;
**   }
*/
typedef struct {
  int       line;
  CallKind  kind;
  const char *callee;   /* owned */
  int       arg_count;  /* -1 = variable (B==0) */
  int       is_tail;    /* bool */
} CallSite;

/*
** ReadEntry — one entry per OP_GETTABUP or OP_GETFIELD that reads from
** _ENV or a known table register.
**
**   message ReadEntry {
**     string table_name  = 1;  // "_ENV" for globals, or upvalue/register name
**     string field_name  = 2;
**   }
*/
typedef struct {
  const char *table_name;  /* owned */
  const char *field_name;  /* owned */
} ReadEntry;

/*
** FunctionInfo — one entry per Proto, including nested ones.
** Maps to a proto message:
**
**   message FunctionInfo {
**     string         source              = 1;
**     int32          line_defined        = 2;
**     int32          last_line           = 3;
**     int32          param_count         = 4;
**     bool           is_vararg           = 5;
**     bool           is_vararg_used      = 6;
**     bool           is_method           = 7;
**     repeated string param_names        = 8;
**     repeated string upvalue_names      = 9;
**     ReturnKind     return_kind         = 10;
**     TableInfo      table_info          = 11;
**     repeated ClosureInfo closures      = 12;
**     repeated ConstantEntry constants   = 13;
**     repeated int32 child_proto_indices = 14;
**     repeated CallSite call_sites       = 15;
**     repeated ReadEntry reads           = 16;
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
  int         is_vararg_used;     /* OP_VARARG actually appears in bytecode */
  int         is_method;          /* first param is "self" */
  const char **param_names;       /* [param_count] */
  int         upvalue_count;
  const char **upvalue_names;     /* [upvalue_count] */

  /* return analysis */
  ReturnKind  return_kind;
  TableInfo   table_info;         /* valid iff return_kind == RETURN_KIND_TABLE */
  int had_real_return;

  /* closure tracking */
  ClosureInfo *closures;
  int          num_closures;
  int          cap_closures;

  /* constant pool */
  ConstantEntry *constants;
  int            num_constants;

  /* sub-proto hierarchy: indices into report->functions[] of direct children */
  int  *child_proto_indices;
  int   num_children;
  int   cap_children;

  /* call site tracking */
  CallSite *call_sites;
  int       num_call_sites;
  int       cap_call_sites;

  /* global/field reads (_ENV and one-level GETFIELD) */
  ReadEntry *reads;
  int        num_reads;
  int        cap_reads;

  /* pending resolution: proto pointers needing function_index after recursion */
  const Proto **pending_proto;   /* parallel to globals, indexed by global slot */
  int           num_pending;     /* always == report->num_globals at resolution time */
} FunctionInfo;

/*
** GlobalEntry — a name set in _ENV at top level.
** Maps to a proto message:
**
**   message GlobalEntry {
**     string name           = 1;
**     bool   is_function    = 2;
**     int32  function_index = 3;  // index into report.functions[], -1 if unknown
**   }
*/
typedef struct {
  const char *name;
  int         is_function;
  int         function_index;  /* -1 = not resolved */
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

static void push_child_index(FunctionInfo *fi, int idx) {
  if (fi->num_children >= fi->cap_children) {
    int nc = fi->cap_children == 0 ? 4 : fi->cap_children * 2;
    fi->child_proto_indices = (int *)realloc(fi->child_proto_indices,
                                              nc * sizeof(int));
    fi->cap_children = nc;
  }
  fi->child_proto_indices[fi->num_children++] = idx;
}

static CallSite *push_call_site(FunctionInfo *fi) {
  if (fi->num_call_sites >= fi->cap_call_sites) {
    int nc = fi->cap_call_sites == 0 ? 8 : fi->cap_call_sites * 2;
    fi->call_sites = (CallSite *)realloc(fi->call_sites, nc * sizeof(CallSite));
    fi->cap_call_sites = nc;
  }
  CallSite *cs = &fi->call_sites[fi->num_call_sites++];
  memset(cs, 0, sizeof(CallSite));
  return cs;
}

static void push_read(FunctionInfo *fi, const char *tbl, const char *field) {
  /* Deduplicate — identical table.field pairs are noise when read in a loop */
  for (int i = 0; i < fi->num_reads; i++) {
    if (strcmp(fi->reads[i].table_name, tbl) == 0 &&
        strcmp(fi->reads[i].field_name, field) == 0)
      return;
  }
  if (fi->num_reads >= fi->cap_reads) {
    int nc = fi->cap_reads == 0 ? 8 : fi->cap_reads * 2;
    fi->reads = (ReadEntry *)realloc(fi->reads, nc * sizeof(ReadEntry));
    fi->cap_reads = nc;
  }
  fi->reads[fi->num_reads].table_name = str_dup(tbl);
  fi->reads[fi->num_reads].field_name = str_dup(field);
  fi->num_reads++;
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
static void upsert_global(InterfaceReport *report, const char *name,
                           int is_fn, int function_index) {
  for (int i = 0; i < report->num_globals; i++) {
    if (strcmp(report->globals[i].name, name) == 0) {
      if (is_fn) report->globals[i].is_function = 1;
      if (function_index >= 0)
        report->globals[i].function_index = function_index;
      return;
    }
  }
  if (report->num_globals >= report->cap_globals) {
    int nc = report->cap_globals == 0 ? 8 : report->cap_globals * 2;
    report->globals = (GlobalEntry *)realloc(report->globals,
                                              nc * sizeof(GlobalEntry));
    report->cap_globals = nc;
  }
  report->globals[report->num_globals].name           = str_dup(name);
  report->globals[report->num_globals].is_function    = is_fn;
  report->globals[report->num_globals].function_index = function_index;
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
      if (pop == OP_GETTABUP || pop == OP_GETTABLE ||
          pop == OP_GETFIELD || pop == OP_GETI)  return RETURN_KIND_UPVALUE;
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
** Resolve the callee name for a CALL/TAILCALL at `call_pc`.
** The callee is in R[callee_reg].  Walk back to find what loaded it.
**
** Sets *kind_out and writes a heap-allocated name string into *name_out
** (caller owns it).  name_out may be set to NULL for CALL_KIND_UNKNOWN.
**
** Resolution rules:
**   GETTABUP  upv=0, K[C]=string  → CALL_KIND_GLOBAL,  name = K[C]
**   GETTABUP  upv!=0, K[C]=string → CALL_KIND_FIELD,   name = upvname.K[C]
**   GETFIELD  -, K[C]=string      → CALL_KIND_FIELD,   name = ?.K[C]
**   SELF      -, K[C]=string      → CALL_KIND_METHOD,  name = ?:K[C]
**   MOVE / other                  → CALL_KIND_LOCAL,   name = NULL
** ------------------------------------------------------------------------- */
static void resolve_callee(const Proto *f, int call_pc, int callee_reg,
                           CallKind *kind_out, const char **name_out) {
  *kind_out = CALL_KIND_UNKNOWN;
  *name_out = NULL;

  int limit = (call_pc - 32 < 0) ? 0 : call_pc - 32;

  for (int i = call_pc - 1; i >= limit; i--) {
    Instruction ins = f->code[i];
    OpCode op = GET_OPCODE(ins);
    int a = GETARG_A(ins);

    if (a != callee_reg) continue;

    if (op == OP_GETTABUP) {
      int upv     = GETARG_B(ins);
      int key_idx = GETARG_C(ins);
      if (key_idx >= f->sizek) break;
      TValue *kv = &f->k[key_idx];
      if (!ttisstring(kv)) break;
      const char *field = getstr(tsvalue(kv));

      if (upv == 0) {
        /* Direct _ENV access → global call */
        *kind_out = CALL_KIND_GLOBAL;
        *name_out = str_dup(field);
      } else {
        /* Named upvalue → "upvname.field" */
        const char *upvname = "?";
        if (upv < f->sizeupvalues && f->upvalues[upv].name)
          upvname = getstr(f->upvalues[upv].name);
        size_t len = strlen(upvname) + 1 + strlen(field) + 1;
        char *buf = (char *)malloc(len);
        snprintf(buf, len, "%s.%s", upvname, field);
        *kind_out = CALL_KIND_FIELD;
        *name_out = buf;
      }
      return;
    }

    if (op == OP_GETFIELD) {
      int key_idx = GETARG_C(ins);
      if (key_idx >= f->sizek) break;
      TValue *kv = &f->k[key_idx];
      if (!ttisstring(kv)) break;
      const char *field = getstr(tsvalue(kv));

      /* Try to identify the source register (B) as an upvalue name */
      int src_reg = GETARG_B(ins);
      const char *src_name = "?";
      /* Walk back a little further to see if src_reg came from GETTABUP */
      int limit2 = (i - 16 < 0) ? 0 : i - 16;
      for (int j = i - 1; j >= limit2; j--) {
        Instruction prev = f->code[j];
        if (GET_OPCODE(prev) == OP_GETTABUP && GETARG_A(prev) == src_reg) {
          int cidx = GETARG_C(prev);
          if (cidx < f->sizek && ttisstring(&f->k[cidx]))
            src_name = getstr(tsvalue(&f->k[cidx]));
          break;
        }
        if (GETARG_A(prev) == src_reg) break; /* overwritten, give up */
      }

      size_t len = strlen(src_name) + 1 + strlen(field) + 1;
      char *buf = (char *)malloc(len);
      snprintf(buf, len, "%s.%s", src_name, field);
      *kind_out = CALL_KIND_FIELD;
      *name_out = buf;
      return;
    }

    if (op == OP_SELF) {
      int key_idx = GETARG_C(ins);
      if (key_idx >= f->sizek) break;
      TValue *kv = &f->k[key_idx];
      if (!ttisstring(kv)) break;
      *kind_out = CALL_KIND_METHOD;
      *name_out = str_dup(getstr(tsvalue(kv)));
      return;
    }

    if (op == OP_MOVE || op == OP_GETUPVAL) {
      *kind_out = CALL_KIND_LOCAL;
      return;
    }

    if (op == OP_CLOSURE) {
      *kind_out = CALL_KIND_LOCAL;
      return;
    }

    /* Any other writer — unknown */
    break;
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

  /* --- Constant pool ---------------------------------------------------- */
  fi->num_constants = f->sizek;
  if (f->sizek > 0) {
    fi->constants = (ConstantEntry *)calloc(f->sizek, sizeof(ConstantEntry));
    for (int i = 0; i < f->sizek; i++) {
      TValue *tv = &f->k[i];
      ConstantEntry *ce = &fi->constants[i];
      if (ttisstring(tv)) {
        ce->kind  = CONST_KIND_STRING;
        ce->s_val = str_dup(getstr(tsvalue(tv)));
      } else if (ttisinteger(tv)) {
        ce->kind  = CONST_KIND_INTEGER;
        ce->i_val = ivalue(tv);
      } else if (ttisfloat(tv)) {
        ce->kind  = CONST_KIND_FLOAT;
        ce->f_val = fltvalue(tv);
      } else if (ttistrue(tv) && ttisboolean(tv)) {
        ce->kind  = CONST_KIND_BOOL;
        ce->b_val = 1;
      } else if (ttisboolean(tv)) {
        ce->kind  = CONST_KIND_BOOL;
        ce->b_val = 0;
      } else {
        /* LUA_TNIL or anything unrecognised */
        ce->kind  = CONST_KIND_NULL;
      }
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

        /* Update return_kind:
        **   - UNKNOWN / VOID are weak: any stronger kind overwrites them.
        **   - If we already have a non-weak kind and see a *different*
        **     non-weak kind, the function has mixed return types → MIXED.
        **   - Once MIXED, stop updating.
        **   - VOID produced by the compiler's trailing RETURN0 guard must
        **     not clobber a real kind already recorded. */
        if (fi->return_kind != RETURN_KIND_MIXED) {
          int cur_weak = (fi->return_kind == RETURN_KIND_UNKNOWN ||
                          fi->return_kind == RETURN_KIND_VOID);
          int new_weak = (kind == RETURN_KIND_UNKNOWN ||
                          kind == RETURN_KIND_VOID);

          if (cur_weak && !new_weak) {
            /* Real return kind found — always take it */
            fi->return_kind = kind;
          } else if (!cur_weak && !new_weak && kind != fi->return_kind) {
            /* Two different real return kinds — mixed */
            fi->return_kind = RETURN_KIND_MIXED;
          } else if (fi->return_kind == RETURN_KIND_UNKNOWN && new_weak
                     && kind == RETURN_KIND_VOID) {
            /* Only promote UNKNOWN→VOID if we haven't seen any real return
            ** site yet; tracked via a flag so the trailing RETURN0 guard
            ** doesn't erase legitimate UNKNOWN from a real return path */
            if (!fi->had_real_return)
              fi->return_kind = RETURN_KIND_VOID;
          }
        }
        /* Track whether any non-void, non-unknown return site was seen */
        if (kind != RETURN_KIND_UNKNOWN && kind != RETURN_KIND_VOID)
          fi->had_real_return = 1;

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
        ** We only care about _ENV (upvalue 0) assignments at the top level.
        ** When k==1, C is a constant index (not a register) — the value is
        ** a literal, never a closure. */
        if (GETARG_A(ins) != 0) break;

        int key_idx = GETARG_B(ins);
        int val_reg = GETARG_C(ins);
        int k_flag  = GETARG_k(ins);

        if (key_idx >= f->sizek) break;
        TValue *kv = &f->k[key_idx];
        if (!ttisstring(kv)) break;

        const char *name = getstr(tsvalue(kv));

        int         is_fn      = 0;
        const Proto *child_proto = NULL;

        if (!k_flag) {
          int limit2 = (pc - 16 < 0) ? 0 : pc - 16;
          for (int j = pc - 1; j >= limit2; j--) {
            Instruction prev = f->code[j];
            if (GET_OPCODE(prev) == OP_CLOSURE && GETARG_A(prev) == val_reg) {
              is_fn = 1;
              int bx = GETARG_Bx(prev);
              if (bx < f->sizep)
                child_proto = f->p[bx];
              break;
            }
            if (GETARG_A(prev) == val_reg) break;
          }
        }

        /* Record the global now; function_index resolved after recursion */
        int global_slot = report->num_globals;
        upsert_global(report, name, is_fn, -1);

        /* If this was a new entry (not a duplicate) and we have a proto
        ** pointer, stash it for post-recursion resolution */
        if (child_proto && report->num_globals > global_slot) {
          /* grow pending_proto to match globals array */
          fi->pending_proto = (const Proto **)realloc(fi->pending_proto,
                                report->num_globals * sizeof(const Proto *));
          /* fill any gap for entries added without a proto */
          for (int g = fi->num_pending; g < global_slot; g++)
            fi->pending_proto[g] = NULL;
          fi->pending_proto[global_slot] = child_proto;
          fi->num_pending = report->num_globals;
        }
        break;
      }

      case OP_VARARG: {
        fi->is_vararg_used = 1;
        break;
      }

      case OP_GETTABUP: {
        /* R[A] := UpValue[B][K[C]:shortstring]
        ** Record reads from any upvalue where C is a string constant.
        ** Upvalue 0 is _ENV (globals); others are named captures. */
        int upv     = GETARG_B(ins);
        int key_idx = GETARG_C(ins);
        if (key_idx < f->sizek) {
          TValue *kv = &f->k[key_idx];
          if (ttisstring(kv)) {
            const char *field = getstr(tsvalue(kv));
            const char *tbl   = "_ENV";
            if (upv != 0 && upv < f->sizeupvalues && f->upvalues[upv].name)
              tbl = getstr(f->upvalues[upv].name);
            push_read(fi, tbl, field);
          }
        }
        break;
      }

      case OP_GETFIELD: {
        /* R[A] := R[B][K[C]:shortstring]
        ** Record one-level field reads where K[C] is a string. */
        int key_idx = GETARG_C(ins);
        if (key_idx < f->sizek) {
          TValue *kv = &f->k[key_idx];
          if (ttisstring(kv)) {
            push_read(fi, "?", getstr(tsvalue(kv)));
          }
        }
        break;
      }

      case OP_CALL:
      case OP_TAILCALL: {
        int callee_reg = GETARG_A(ins);
        int b          = GETARG_B(ins);
        int is_tail    = (op == OP_TAILCALL) ? 1 : 0;

        CallSite *cs = push_call_site(fi);
        cs->is_tail  = is_tail;
        /* arg_count: B-1 args when B>0, variable when B==0 */
        cs->arg_count = (b == 0) ? -1 : (b - 1);

        /* Best-effort line number: use the absolute line info table if
        ** present (stripped bytecode omits it, in which case we emit 0). */
        cs->line = 0;
        if (f->abslineinfo && f->sizeabslineinfo > 0) {
          /* abslineinfo is a sorted array of {pc, line} checkpoints.
          ** Find the last entry with pc <= current pc. */
          int lo = 0, hi = f->sizeabslineinfo - 1, best = 0;
          while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (f->abslineinfo[mid].pc <= pc) {
              best = f->abslineinfo[mid].line;
              lo = mid + 1;
            } else {
              hi = mid - 1;
            }
          }
          cs->line = best;
        } else if (f->lineinfo && pc < f->sizecode) {
          /* Compact delta encoding: lineinfo[i] is a signed byte offset
          ** from the previous line. Walk from the start. */
          int line = f->linedefined;
          for (int k = 0; k <= pc && k < f->sizecode; k++)
            line += (signed char)f->lineinfo[k];
          cs->line = line;
        }

        resolve_callee(f, pc, callee_reg, &cs->kind, &cs->callee);
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
  int my_index = report->num_functions - 1;

  for (int i = 0; i < f->sizep; i++) {
    int child_index = report->num_functions;
    push_child_index(&report->functions[my_index], child_index);
    analyze_proto_recursive(f->p[i], report);
  }

  /* --- Resolve pending global → function_index mappings ---------------- */
  /* Now that all child protos have been pushed into report->functions,
  ** match each stashed Proto pointer against the recorded FunctionInfo
  ** entries by pointer identity on line_defined + source. Direct pointer
  ** comparison against f->p[bx] would be ideal but FunctionInfo doesn't
  ** store the Proto*; matching on linedefined is unambiguous for any
  ** well-formed script (two sibling functions on the same line is
  ** pathological). */
  fi = &report->functions[my_index]; /* re-fetch: may have moved due to realloc */
  if (fi->num_pending > 0) {
    for (int g = 0; g < fi->num_pending && g < report->num_globals; g++) {
      if (!fi->pending_proto[g]) continue;
      int target_line = fi->pending_proto[g]->linedefined;
      for (int k = 0; k < report->num_functions; k++) {
        if (report->functions[k].line_defined == target_line) {
          report->globals[g].function_index = k;
          break;
        }
      }
    }
    free(fi->pending_proto);
    fi->pending_proto = NULL;
    fi->num_pending   = 0;
  }
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

static void write_constant_array(FILE *out, const ConstantEntry *arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    const ConstantEntry *ce = &arr[i];
    write_indent(out, depth + 1);
    fprintf(out, "{\"kind\": %d, ", (int)ce->kind);
    switch (ce->kind) {
      case CONST_KIND_STRING:
        fprintf(out, "\"s_val\": ");
        json_write_string(out, ce->s_val);
        fprintf(out, ", \"i_val\": 0, \"f_val\": 0.0, \"b_val\": false");
        break;
      case CONST_KIND_INTEGER:
        fprintf(out, "\"s_val\": null, \"i_val\": " LUA_INTEGER_FMT
                     ", \"f_val\": 0.0, \"b_val\": false",
                (LUAI_UACINT)ce->i_val);
        break;
      case CONST_KIND_FLOAT:
        fprintf(out, "\"s_val\": null, \"i_val\": 0, \"f_val\": %.17g"
                     ", \"b_val\": false",
                (double)ce->f_val);
        break;
      case CONST_KIND_BOOL:
        fprintf(out, "\"s_val\": null, \"i_val\": 0, \"f_val\": 0.0"
                     ", \"b_val\": %s", ce->b_val ? "true" : "false");
        break;
      default: /* CONST_KIND_NULL */
        fprintf(out, "\"s_val\": null, \"i_val\": 0, \"f_val\": 0.0"
                     ", \"b_val\": false");
        break;
    }
    fputc('}', out);
    if (i < n - 1) fputc(',', out);
    fputc('\n', out);
  }
  write_indent(out, depth);
  fputc(']', out);
}

static void write_int_array(FILE *out, const int *arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    write_indent(out, depth + 1);
    fprintf(out, "%d", arr[i]);
    if (i < n - 1) fputc(',', out);
    fputc('\n', out);
  }
  write_indent(out, depth);
  fputc(']', out);
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

static void write_call_site_array(FILE *out, const CallSite *arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    const CallSite *cs = &arr[i];
    write_indent(out, depth + 1);
    fprintf(out, "{\"line\": %d, \"kind\": %d, \"callee\": ",
            cs->line, (int)cs->kind);
    json_write_string(out, cs->callee ? cs->callee : "");
    fprintf(out, ", \"arg_count\": %d, \"is_tail\": %s}",
            cs->arg_count, cs->is_tail ? "true" : "false");
    if (i < n - 1) fputc(',', out);
    fputc('\n', out);
  }
  write_indent(out, depth);
  fputc(']', out);
}

static void write_read_array(FILE *out, const ReadEntry *arr, int n, int depth) {
  fprintf(out, "[\n");
  for (int i = 0; i < n; i++) {
    write_indent(out, depth + 1);
    fprintf(out, "{\"table_name\": ");
    json_write_string(out, arr[i].table_name);
    fprintf(out, ", \"field_name\": ");
    json_write_string(out, arr[i].field_name);
    fputc('}', out);
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
  fprintf(out, "\"is_vararg_used\": %s,\n", fi->is_vararg_used ? "true" : "false");

  write_indent(out, depth + 1);
  fprintf(out, "\"closures\": ");
  write_closure_array(out, fi->closures, fi->num_closures, depth + 1);
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"constants\": ");
  write_constant_array(out, fi->constants, fi->num_constants, depth + 1);
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"child_proto_indices\": ");
  write_int_array(out, fi->child_proto_indices, fi->num_children, depth + 1);
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"call_sites\": ");
  write_call_site_array(out, fi->call_sites, fi->num_call_sites, depth + 1);
  fprintf(out, ",\n");

  write_indent(out, depth + 1);
  fprintf(out, "\"reads\": ");
  write_read_array(out, fi->reads, fi->num_reads, depth + 1);
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
    fprintf(out, ", \"is_function\": %s, \"function_index\": %d}",
            report->globals[i].is_function ? "true" : "false",
            report->globals[i].function_index);
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
    for (int j = 0; j < fi->num_constants; j++)
      free((void *)fi->constants[j].s_val);
    free(fi->constants);
    free(fi->child_proto_indices);
    free(fi->pending_proto);
    for (int j = 0; j < fi->num_call_sites; j++)
      free((void *)fi->call_sites[j].callee);
    free(fi->call_sites);
    for (int j = 0; j < fi->num_reads; j++) {
      free((void *)fi->reads[j].table_name);
      free((void *)fi->reads[j].field_name);
    }
    free(fi->reads);
  }
  free(report->functions);

  for (int i = 0; i < report->num_globals; i++)
    free((void *)report->globals[i].name);
  free(report->globals);

  free(report);
}