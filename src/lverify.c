/*
** $Id: lverify.c $
** Load-time bytecode verifier
** See Copyright Notice in lua.h
*/

#define lverify_c
#define LUA_CORE

#include "lprefix.h"


#include "lua.h"

#include "ldo.h"
#include "llimits.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltm.h"
#include "lverify.h"


/*
** Prototypes nest, and this walk is recursive, so it needs a bound of
** its own rather than the C stack's. (The loader recurses over the same
** structure while reading it, so a chunk deep enough to reach this has
** already been through loadFunction at the same depth.)
*/
#define MAXPROTODEPTH	200


/*
** The arithmetic and bitwise opcodes, in all three operand shapes
** (register, constant, immediate), occupy one contiguous run of the
** opcode enum. Each is followed by the metamethod instruction it falls
** through to; the VM skips that instruction when the fast path
** succeeds, so it is part of the encoding rather than an optimisation.
** testMMMode marks the metamethod instructions themselves, not these.
*/
#define isarith(o)	(OP_ADDI <= (o) && (o) <= OP_SHR)


typedef struct {
  lua_State *L;
  const char *src;  /* chunk name, for the message */
  const Proto *f;  /* prototype being checked */
  int pc;  /* instruction being checked */
} VState;


/*
** Deliberately the loader's own wording: which half of the load refused
** a malformed chunk is not something a caller can act on.
*/
static l_noret verifyerror (VState *V, const char *why) {
  luaO_pushfstring(V->L, "%s: bad binary format (%s at instruction %d)",
                   V->src, why, V->pc);
  luaD_throw(V->L, LUA_ERRSYNTAX);
}


static void vfycheck (VState *V, int cond, const char *why) {
  if (l_unlikely(!cond))
    verifyerror(V, why);
}


/*
** R[r] must exist. Registers run 0 .. maxstacksize-1.
*/
static void vfyreg (VState *V, int r) {
  vfycheck(V, 0 <= r && r < V->f->maxstacksize, "register out of range");
}


/*
** 'r' is the exclusive end of a register window -- an instruction that
** sets L->top to base+r may name one past the last register, but no
** further, or top leaves the frame the call allocated.
*/
static void vfywindow (VState *V, int r) {
  vfycheck(V, 0 <= r && r <= V->f->maxstacksize, "register window out of range");
}


static void vfyk (VState *V, int idx) {
  vfycheck(V, 0 <= idx && idx < V->f->sizek, "constant index out of range");
}


/*
** The VM reads these constants as strings without testing the tag, so
** the tag is this pass's responsibility.
*/
static void vfykstr (VState *V, int idx) {
  vfyk(V, idx);
  vfycheck(V, ttisstring(&V->f->k[idx]), "field name constant is not a string");
}


static void vfyupval (VState *V, int idx) {
  vfycheck(V, 0 <= idx && idx < V->f->sizeupvalues, "upvalue index out of range");
}


/*
** RK(C): a constant when the k flag is set, a register otherwise.
*/
static void vfyRKC (VState *V, Instruction i) {
  if (TESTARG_k(i))
    vfyk(V, GETARG_C(i));
  else
    vfyreg(V, GETARG_C(i));
}


/*
** A jump target. Landing on an OP_EXTRAARG would execute a continuation
** word as though it were an instruction; nothing the compiler emits ever
** does that.
*/
static void vfydest (VState *V, int target) {
  vfycheck(V, 0 <= target && target < V->f->sizecode, "jump out of range");
  vfycheck(V, GET_OPCODE(V->f->code[target]) != OP_EXTRAARG,
        "jump into an extra argument");
}


/*
** Instructions that consume the following word as an operand. NEWTABLE
** and LOADKX always do; SETLIST only when its k flag is set.
*/
static void vfyextraarg (VState *V, int pc) {
  vfycheck(V, pc + 1 < V->f->sizecode, "missing extra argument");
  vfycheck(V, GET_OPCODE(V->f->code[pc + 1]) == OP_EXTRAARG,
        "extra argument expected");
}


/*
** A test skips the following instruction when it does not branch, so
** both that instruction and the one after it have to exist: execution
** resumes at pc+2.
*/
static void vfyskip (VState *V, int pc) {
  vfycheck(V, pc + 2 < V->f->sizecode, "instruction skips past the end of code");
}


/*
** The C operand of OP_RETURN and OP_TAILCALL is not a count but a
** signal: zero, or the parameter count of a function with hidden vararg
** arguments (lcode.c, luaK_finish). The VM subtracts it from ci->func,
** so an arbitrary value walks the stack pointer backwards out of the
** frame -- a write, not just a read.
*/
static void vfyvaparams (VState *V, int c) {
  vfycheck(V, c == 0 || c == V->f->numparams + 1,
        "bad vararg parameter count in return");
}


static void verifyinstruction (VState *V, int pc) {
  const Proto *f = V->f;
  Instruction i = f->code[pc];
  OpCode op = GET_OPCODE(i);
  int a = GETARG_A(i);
  V->pc = pc;
  vfycheck(V, cast_int(op) < NUM_OPCODES, "unknown opcode");
  /* every opcode that assigns to R[A] needs A to name a register; the
     opcode table already says which those are */
  if (testAMode(op))
    vfyreg(V, a);
  /* an arithmetic opcode is always followed by its metamethod fallback:
     the VM either executes that instruction or skips it, so something
     else in the slot is executed as a metamethod call or silently
     skipped */
  if (isarith(op)) {
    vfycheck(V, pc + 1 < f->sizecode, "missing metamethod instruction");
    vfycheck(V, testMMMode(GET_OPCODE(f->code[pc + 1])),
          "arithmetic opcode not followed by its metamethod instruction");
  }
  switch (op) {
    case OP_MOVE: case OP_UNM: case OP_BNOT: case OP_NOT: case OP_LEN: {
      vfyreg(V, GETARG_B(i));
      break;
    }
    case OP_LOADI: case OP_LOADF: case OP_LOADFALSE: case OP_LOADTRUE: {
      break;  /* A only, already checked */
    }
    case OP_LFALSESKIP: {
      vfyskip(V, pc);  /* unconditional pc++ */
      break;
    }
    case OP_LOADK: {
      vfyk(V, GETARG_Bx(i));
      break;
    }
    case OP_LOADKX: {
      vfyextraarg(V, pc);
      vfyk(V, GETARG_Ax(f->code[pc + 1]));
      break;
    }
    case OP_LOADNIL: {
      vfyreg(V, a + GETARG_B(i));  /* clears R[A] .. R[A+B] inclusive */
      break;
    }
    case OP_GETUPVAL: case OP_SETUPVAL: {
      vfyreg(V, a);  /* SETUPVAL reads R[A]; it is not an A-mode opcode */
      vfyupval(V, GETARG_B(i));
      break;
    }
    case OP_GETTABUP: {
      vfyupval(V, GETARG_B(i));
      vfykstr(V, GETARG_C(i));
      break;
    }
    case OP_GETTABLE: {
      vfyreg(V, GETARG_B(i));
      vfyreg(V, GETARG_C(i));
      break;
    }
    case OP_GETI: {
      vfyreg(V, GETARG_B(i));
      break;  /* C is an immediate integer key */
    }
    case OP_GETFIELD: {
      vfyreg(V, GETARG_B(i));
      vfykstr(V, GETARG_C(i));
      break;
    }
    case OP_SETTABUP: {
      vfyupval(V, a);
      vfykstr(V, GETARG_B(i));
      vfyRKC(V, i);
      break;
    }
    case OP_SETTABLE: {
      vfyreg(V, a);
      vfyreg(V, GETARG_B(i));
      vfyRKC(V, i);
      break;
    }
    case OP_SETI: {
      vfyreg(V, a);
      vfyRKC(V, i);
      break;  /* B is an immediate integer key */
    }
    case OP_SETFIELD: {
      vfyreg(V, a);
      vfykstr(V, GETARG_B(i));
      vfyRKC(V, i);
      break;
    }
    case OP_NEWTABLE: {
      vfyextraarg(V, pc);  /* the VM skips it whether or not k is set */
      break;
    }
    case OP_SELF: {
      vfyreg(V, a + 1);  /* writes R[A+1] as well as R[A] */
      vfyreg(V, GETARG_B(i));
      vfykstr(V, GETARG_C(i));
      break;
    }
    case OP_ADDI: case OP_SHLI: case OP_SHRI: {
      vfyreg(V, GETARG_B(i));
      break;  /* C is an immediate operand */
    }
    case OP_ADDK: case OP_SUBK: case OP_MULK: case OP_MODK:
    case OP_POWK: case OP_DIVK: case OP_IDIVK:
    case OP_BANDK: case OP_BORK: case OP_BXORK: {
      vfyreg(V, GETARG_B(i));
      vfyk(V, GETARG_C(i));
      break;
    }
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
    case OP_POW: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
      vfyreg(V, GETARG_B(i));
      vfyreg(V, GETARG_C(i));
      break;
    }
    case OP_MMBIN: case OP_MMBINI: case OP_MMBINK: {
      /* the fallback reads the instruction before it, and takes that
         instruction's A as the register to write the result into */
      vfycheck(V, pc >= 1, "metamethod instruction with nothing before it");
      vfycheck(V, isarith(GET_OPCODE(f->code[pc - 1])),
            "metamethod instruction does not follow an arithmetic opcode");
      vfyreg(V, a);
      if (op == OP_MMBIN)
        vfyreg(V, GETARG_B(i));
      else if (op == OP_MMBINK)
        vfyk(V, GETARG_B(i));
      /* C selects a metamethod name out of a fixed-size table */
      vfycheck(V, 0 <= GETARG_C(i) && GETARG_C(i) < TM_N,
            "metamethod index out of range");
      break;
    }
    case OP_CONCAT: {
      /* concatenates R[A] .. R[A+B-1], and sets top to base+A+B */
      vfycheck(V, GETARG_B(i) >= 1, "concat of fewer than one value");
      vfywindow(V, a + GETARG_B(i));
      break;
    }
    case OP_CLOSE: case OP_TBC: {
      vfyreg(V, a);
      break;
    }
    case OP_JMP: {
      vfydest(V, pc + 1 + GETARG_sJ(i));
      break;
    }
    case OP_EQ: case OP_LT: case OP_LE: case OP_TESTSET: {
      vfyreg(V, a);
      vfyreg(V, GETARG_B(i));
      vfyskip(V, pc);
      break;
    }
    case OP_EQK: {
      vfyreg(V, a);
      vfyk(V, GETARG_B(i));
      vfyskip(V, pc);
      break;
    }
    case OP_EQI: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
    case OP_TEST: {
      vfyreg(V, a);
      vfyskip(V, pc);
      break;
    }
    case OP_CALL: {
      int b = GETARG_B(i), c = GETARG_C(i);
      vfyreg(V, a);
      if (b != 0)  /* fixed argument count: top becomes base+A+B */
        vfywindow(V, a + b);
      if (c != 0)  /* fixed result count: results land in R[A..A+C-2] */
        vfywindow(V, a + c - 1);
      break;
    }
    case OP_TAILCALL: {
      int b = GETARG_B(i);
      vfyreg(V, a);
      if (b != 0)
        vfywindow(V, a + b);
      vfyvaparams(V, GETARG_C(i));
      break;
    }
    case OP_RETURN: {
      int b = GETARG_B(i);
      vfyreg(V, a);
      if (b != 0)  /* returns R[A] .. R[A+B-2]; top becomes base+A+B-1 */
        vfywindow(V, a + b - 1);
      vfyvaparams(V, GETARG_C(i));
      break;
    }
    case OP_RETURN0: {
      break;
    }
    case OP_RETURN1: {
      vfyreg(V, a);
      break;
    }
    case OP_FORLOOP: case OP_TFORLOOP: {
      /* both jump backwards by Bx */
      vfyreg(V, a + (op == OP_FORLOOP ? 3 : 2));
      vfydest(V, pc + 1 - GETARG_Bx(i));
      break;
    }
    case OP_FORPREP: {
      vfyreg(V, a + 3);
      vfydest(V, pc + 1 + GETARG_Bx(i) + 1);
      break;
    }
    case OP_TFORPREP: {
      /* jumps to the OP_TFORCALL that closes the loop and fetches it
         directly, without going back through the dispatch table */
      int target = pc + 1 + GETARG_Bx(i);
      vfyreg(V, a + 3);
      vfydest(V, target);
      vfycheck(V, GET_OPCODE(f->code[target]) == OP_TFORCALL,
            "generic-for prep does not jump to its call");
      vfycheck(V, GETARG_A(f->code[target]) == a,
            "generic-for prep and call disagree about the loop base");
      break;
    }
    case OP_TFORCALL: {
      /* shuffles the control variables up to R[A+5] before the call,
         then writes C results starting at R[A+4] */
      vfyreg(V, a + 5);
      if (GETARG_C(i) != 0)
        vfyreg(V, a + 3 + GETARG_C(i));
      vfycheck(V, pc + 1 < f->sizecode, "generic-for call with no loop after it");
      vfycheck(V, GET_OPCODE(f->code[pc + 1]) == OP_TFORLOOP,
            "generic-for call not followed by its loop");
      vfycheck(V, GETARG_A(f->code[pc + 1]) == a,
            "generic-for call and loop disagree about the loop base");
      break;
    }
    case OP_SETLIST: {
      vfyreg(V, a);
      if (GETARG_vB(i) != 0)  /* fixed count: reads R[A+1] .. R[A+vB] */
        vfyreg(V, a + GETARG_vB(i));
      if (TESTARG_k(i))
        vfyextraarg(V, pc);
      break;
    }
    case OP_CLOSURE: {
      vfycheck(V, 0 <= GETARG_Bx(i) && GETARG_Bx(i) < f->sizep,
            "prototype index out of range");
      break;
    }
    case OP_VARARG: {
      int c = GETARG_C(i);
      if (c != 0)  /* fixed result count: results land in R[A..A+C-2] */
        vfywindow(V, a + c - 1);
      break;
    }
    case OP_GETVARG: {
      vfyreg(V, GETARG_C(i));
      break;
    }
    case OP_ERRNNIL: {
      /* names the offending global as K[Bx-1]; zero means "no name" */
      int bx = GETARG_Bx(i);
      vfyreg(V, a);
      if (bx != 0)
        vfyk(V, bx - 1);
      break;
    }
    case OP_VARARGPREP: {
      break;  /* takes its counts from the prototype, not from operands */
    }
    case OP_EXTRAARG: {
      /* only ever reached as the operand of the instruction before it,
         which is what makes that instruction's check the real one */
      vfycheck(V, pc >= 1, "extra argument with nothing before it");
      break;
    }
    default: {
      verifyerror(V, "unknown opcode");
    }
  }
}


/*
** A nested prototype's upvalue descriptors are resolved against the
** *enclosing* function: an in-stack upvalue indexes the enclosing
** function's registers, and any other indexes the enclosing function's
** upvalue list. Both are lu_byte, so both fit an instruction operand and
** neither is bounded by anything the loader checked.
*/
static void verifyupvalues (VState *V, const Proto *child) {
  const Proto *encl = V->f;
  int j;
  for (j = 0; j < child->sizeupvalues; j++) {
    Upvaldesc *uv = &child->upvalues[j];
    if (uv->instack)
      vfycheck(V, uv->idx < encl->maxstacksize,
            "nested function captures a register the enclosing function "
            "does not have");
    else
      vfycheck(V, uv->idx < encl->sizeupvalues,
            "nested function captures an upvalue the enclosing function "
            "does not have");
  }
}


static void verifyproto (lua_State *L, const Proto *f, const char *src,
                         int depth) {
  VState V;
  int pc;
  V.L = L;
  V.src = src;
  V.f = f;
  V.pc = 0;
  vfycheck(&V, depth <= MAXPROTODEPTH, "prototypes nested too deeply");
  vfycheck(&V, f->sizecode > 0, "function has no code");
  vfycheck(&V, f->numparams <= f->maxstacksize,
        "more parameters than the function has registers for");
  /* the compiler always closes a function with a return, and without one
     execution runs off the end of the code array */
  switch (GET_OPCODE(f->code[f->sizecode - 1])) {
    case OP_RETURN: case OP_RETURN0: case OP_RETURN1: break;
    default: {
      V.pc = f->sizecode - 1;
      verifyerror(&V, "function does not end in a return");
    }
  }
  for (pc = 0; pc < f->sizecode; pc++)
    verifyinstruction(&V, pc);
  for (pc = 0; pc < f->sizep; pc++) {
    V.pc = 0;
    verifyupvalues(&V, f->p[pc]);
  }
  for (pc = 0; pc < f->sizep; pc++)
    verifyproto(L, f->p[pc], src, depth + 1);
}


void luaU_verify (lua_State *L, const Proto *f) {
  const char *src = (f->source != NULL) ? getstr(f->source) : "?";
  if (*src == '@' || *src == '=')
    src++;  /* the loader strips these too */
  verifyproto(L, f, src, 1);
}
