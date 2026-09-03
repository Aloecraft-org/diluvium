/*
** $Id: ldump.c $
** save precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#define ldump_c
#define LUA_CORE

#include "lprefix.h"


#include <limits.h>
#include <stddef.h>
#include <string.h>  /* Diluvium: memcpy in the secure-dump scramble path */

#include "lua.h"

#include "lapi.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lundump.h"


typedef struct {
  lua_State *L;
  lua_Writer writer;
  void *data;
  size_t offset;  /* current position relative to beginning of dump */
  int strip;
  int status;
  Table *h;  /* table to track saved strings */
  lua_Unsigned nstr;  /* counter for counting saved strings */
  int encrypted;  /* Diluvium: dumping inside a secure (~) function */
} DumpState;


/*
** Diluvium: XOR-scramble a block in place. This is obfuscation, not
** cryptography. Applied to the code and string constants of secure
** functions.
**
** The instruction stream is scrambled iff its own proto is secure, so
** its position in the tree decides it. Strings cannot work that way:
** 5.5 dumps each distinct string once and refers back to it by index
** afterwards, so a string shared between a secure function and ordinary
** code has exactly one stored copy, at whichever site was dumped first.
** That site is not always the secure one -- a function dumps its own
** constants before recursing into its children -- so scrambling by
** position would store such a string in the clear. Instead the dump
** decides per string (see 'taintSecureStrings') and records the decision
** in the string's own header, which is what 'loadString' reads.
**
** The keystream is generated rather than being one repeated byte, for a
** reason that is about the claim rather than about cryptography. A single
** constant means one 'tr' or 'xxd' pass over the whole file recovers
** every hidden string at once, which is barely more than the text editor
** the README says this defends against. Against a keystream an attacker
** has to implement it -- trivial from these public sources, but no longer
** one shell command. That is the whole of the improvement and it should
** not be described as more.
**
** Two properties are load-bearing elsewhere and must survive any future
** change here:
**
**   Deterministic. Identical input must give identical output, with no
**   nonce and no state carried between blocks. 'doc/Messaging.md' plans to
**   content-address prototypes by hashing their stripped dump, which
**   requires byte-identical dumps for identical source. A per-dump nonce
**   would trade this obfuscation for that entire scheme.
**
**   Self-inverse. A second pass restores the original, which is what lets
**   'lundump.c' hold a copy of exactly this function under another name.
**
** The stream is seeded from the block length so that blocks do not all
** share one keystream prefix. Same length plus same contents still gives
** same output, which is what dedup needs.
*/
#define DILUVIUM_XOR_SEED	0xBE5CA1EDu

static void diluvium_scramble (void *data, size_t size) {
  unsigned char *p = (unsigned char *)data;
  l_uint32 k = (DILUVIUM_XOR_SEED ^ cast(l_uint32, size)) & 0xFFFFFFFFu;
  size_t i;
  if (k == 0)  /* xorshift is stuck at zero, which would leave plaintext */
    k = DILUVIUM_XOR_SEED;
  for (i = 0; i < size; i++) {  /* xorshift32 */
    k ^= (k << 13) & 0xFFFFFFFFu;
    k ^= k >> 17;
    k ^= (k << 5) & 0xFFFFFFFFu;
    k &= 0xFFFFFFFFu;
    p[i] ^= cast_uchar(k >> 24);
  }
}


/*
** All high-level dumps go through dumpVector; you can change it to
** change the endianness of the result
*/
#define dumpVector(D,v,n)	dumpBlock(D,v,(n)*sizeof((v)[0]))

#define dumpLiteral(D, s)	dumpBlock(D,s,sizeof(s) - sizeof(char))


/*
** Dump the block of memory pointed by 'b' with given 'size'.
** 'b' should not be NULL, except for the last call signaling the end
** of the dump.
*/
static void dumpBlock (DumpState *D, const void *b, size_t size) {
  if (D->status == 0) {  /* do not write anything after an error */
    lua_unlock(D->L);
    D->status = (*D->writer)(D->L, b, size, D->data);
    lua_lock(D->L);
    D->offset += size;
  }
}


/*
** Dump enough zeros to ensure that current position is a multiple of
** 'align'.
*/
static void dumpAlign (DumpState *D, unsigned align) {
  unsigned padding = align - cast_uint(D->offset % align);
  if (padding < align) {  /* padding == align means no padding */
    static lua_Integer paddingContent = 0;
    lua_assert(align <= sizeof(lua_Integer));
    dumpBlock(D, &paddingContent, padding);
  }
  lua_assert(D->offset % align == 0);
}


#define dumpVar(D,x)		dumpVector(D,&x,1)


static void dumpByte (DumpState *D, int y) {
  lu_byte x = (lu_byte)y;
  dumpVar(D, x);
}


/*
** size for 'dumpVarint' buffer: each byte can store up to 7 bits.
** (The "+6" rounds up the division.)
*/
#define DIBS    ((l_numbits(lua_Unsigned) + 6) / 7)

/*
** Dumps an unsigned integer using the MSB Varint encoding
*/
static void dumpVarint (DumpState *D, lua_Unsigned x) {
  lu_byte buff[DIBS];
  unsigned n = 1;
  buff[DIBS - 1] = x & 0x7f;  /* fill least-significant byte */
  while ((x >>= 7) != 0)  /* fill other bytes in reverse order */
    buff[DIBS - (++n)] = cast_byte((x & 0x7f) | 0x80);
  dumpVector(D, buff + DIBS - n, n);
}


static void dumpSize (DumpState *D, size_t sz) {
  dumpVarint(D, cast(lua_Unsigned, sz));
}


static void dumpInt (DumpState *D, int x) {
  lua_assert(x >= 0);
  dumpVarint(D, cast_uint(x));
}


static void dumpNumber (DumpState *D, lua_Number x) {
  dumpVar(D, x);
}


/*
** Signed integers are coded to keep small values small. (Coding -1 as
** 0xfff...fff would use too many bytes to save a quite common value.)
** A non-negative x is coded as 2x; a negative x is coded as -2x - 1.
** (0 => 0; -1 => 1; 1 => 2; -2 => 3; 2 => 4; ...)
*/
static void dumpInteger (DumpState *D, lua_Integer x) {
  lua_Unsigned cx = (x >= 0) ? 2u * l_castS2U(x)
                             : (2u * ~l_castS2U(x)) + 1;
  dumpVarint(D, cx);
}


/*
** Diluvium: record in 'D->h' that a string will be written from inside a
** secure function somewhere in this chunk, so it is scrambled wherever
** it is written first. 'D->h' doubles as the saved-string table: 0 marks
** a string as secure but not yet written, and a real index (which starts
** at 1) replaces it once it is. See 'dumpString'.
*/
static void taintString (DumpState *D, TString *ts) {
  TValue key, value;
  if (ts == NULL) return;
  setsvalue(D->L, &key, ts);
  setivalue(&value, 0);
  luaH_set(D->L, D->h, &key, &value);  /* h[ts] = 0 */
}


/*
** Walk the whole proto tree before dumping anything and mark every
** string that a secure function will contribute. This mirrors exactly
** the strings 'dumpFunction' writes while 'D->encrypted' holds -- the
** constants, the source and the debug names -- so that the set is
** neither short (which would leak) nor long (which would scramble
** unrelated strings for no reason).
**
** 'secure' is inherited rather than replaced: a child of a secure
** function contributes to it whatever its own flag says, so the fix does
** not depend on the parser having marked every nested proto.
*/
static void taintSecureStrings (DumpState *D, const Proto *f, int secure) {
  int i;
  secure = secure || f->is_encrypted;
  if (secure) {
    for (i = 0; i < f->sizek; i++) {
      const TValue *o = &f->k[i];
      if (ttisstring(o))
        taintString(D, tsvalue(o));
    }
    if (!D->strip) {
      taintString(D, f->source);
      for (i = 0; i < f->sizelocvars; i++)
        taintString(D, f->locvars[i].varname);
      for (i = 0; i < f->sizeupvalues; i++)
        taintString(D, f->upvalues[i].name);
    }
  }
  for (i = 0; i < f->sizep; i++)
    taintSecureStrings(D, f->p[i], secure);
}


/*
** Dump a String. First dump its "size":
** size==0 is followed by an index and means "reuse saved string with
** that index"; index==0 means NULL.
** size>=1 means the string contents follow. Diluvium widens this field
** by one bit -- it is dumped as size*2, plus 1 when the contents are
** scrambled -- because whether a string is hidden is a property of the
** string rather than of the position it is written at. Zero still means
** "reuse", since a written string always has size>=1. The real size does
** not include the ending '\0' (which is not dumped), so adding 1 to it
** cannot overflow a size_t; nor can doubling it, for a string long
** enough to need the top bit could not have been in memory to be dumped.
**
** That the flag is visible tells a reader which strings are hidden, but
** not what they are, and the scramble is avowedly obfuscation rather
** than cryptography -- so this costs nothing that was being relied on.
*/
static void dumpString (DumpState *D, TString *ts) {
  if (ts == NULL) {
    dumpVarint(D, 0);  /* will "reuse" NULL */
    dumpVarint(D, 0);  /* special index for NULL */
  }
  else {
    TValue idx;
    int tag = luaH_getstr(D->h, ts, &idx);
    /* 'h[ts]' is absent when unseen, 0 when marked secure by the taint
       pass but not yet written, and its index once written. */
    if (!tagisempty(tag) && ivalue(&idx) != 0) {  /* string already saved? */
      dumpVarint(D, 0);  /* reuse a saved string */
      dumpVarint(D, l_castS2U(ivalue(&idx)));  /* index of saved string */
    }
    else {  /* must write and save the string */
      TValue key, value;  /* to save the string in the hash */
      size_t size;
      const char *s = getlstr(ts, size);
      /* Diluvium: scramble if this string belongs to a secure function
         anywhere in the chunk, whether or not this is that site. */
      int scrambled = D->encrypted || !tagisempty(tag);
      dumpSize(D, (size + 1) * 2 + cast_sizet(scrambled));
      if (scrambled) {  /* Diluvium: hide strings of secure functions */
        char *buff = luaM_newvector(D->L, size + 1, char);
        memcpy(buff, s, size + 1);
        diluvium_scramble(buff, size + 1);
        dumpVector(D, buff, size + 1);
        luaM_freearray(D->L, buff, size + 1);
      }
      else
        dumpVector(D, s, size + 1);  /* include ending '\0' */
      D->nstr++;  /* one more saved string */
      setsvalue(D->L, &key, ts);  /* the string is the key */
      setivalue(&value, l_castU2S(D->nstr));  /* its index is the value */
      luaH_set(D->L, D->h, &key, &value);  /* h[ts] = nstr */
      /* integer value does not need barrier */
    }
  }
}


static void dumpCode (DumpState *D, const Proto *f) {
  dumpInt(D, f->sizecode);
  dumpAlign(D, sizeof(f->code[0]));
  lua_assert(f->code != NULL);
  if (f->is_encrypted) {  /* Diluvium: hide the instruction stream */
    size_t nbytes = cast_sizet(f->sizecode) * sizeof(f->code[0]);
    Instruction *buff = luaM_newvector(D->L, f->sizecode, Instruction);
    memcpy(buff, f->code, nbytes);
    diluvium_scramble(buff, nbytes);
    dumpVector(D, buff, cast_uint(f->sizecode));
    luaM_freearray(D->L, buff, f->sizecode);
  }
  else
    dumpVector(D, f->code, cast_uint(f->sizecode));
}


static void dumpFunction (DumpState *D, const Proto *f);

static void dumpConstants (DumpState *D, const Proto *f) {
  int i;
  int n = f->sizek;
  dumpInt(D, n);
  for (i = 0; i < n; i++) {
    const TValue *o = &f->k[i];
    int tt = ttypetag(o);
    dumpByte(D, tt);
    switch (tt) {
      case LUA_VNUMFLT:
        dumpNumber(D, fltvalue(o));
        break;
      case LUA_VNUMINT:
        dumpInteger(D, ivalue(o));
        break;
      case LUA_VSHRSTR:
      case LUA_VLNGSTR:
        dumpString(D, tsvalue(o));
        break;
      default:
        lua_assert(tt == LUA_VNIL || tt == LUA_VFALSE || tt == LUA_VTRUE);
    }
  }
}


static void dumpProtos (DumpState *D, const Proto *f) {
  int i;
  int n = f->sizep;
  dumpInt(D, n);
  for (i = 0; i < n; i++)
    dumpFunction(D, f->p[i]);
}


static void dumpUpvalues (DumpState *D, const Proto *f) {
  int i, n = f->sizeupvalues;
  dumpInt(D, n);
  for (i = 0; i < n; i++) {
    dumpByte(D, f->upvalues[i].instack);
    dumpByte(D, f->upvalues[i].idx);
    dumpByte(D, f->upvalues[i].kind);
  }
}


static void dumpDebug (DumpState *D, const Proto *f) {
  int i, n;
  n = (D->strip) ? 0 : f->sizelineinfo;
  dumpInt(D, n);
  if (f->lineinfo != NULL)
    dumpVector(D, f->lineinfo, cast_uint(n));
  n = (D->strip) ? 0 : f->sizeabslineinfo;
  dumpInt(D, n);
  if (n > 0) {
    /* 'abslineinfo' is an array of structures of int's */
    dumpAlign(D, sizeof(int));
    dumpVector(D, f->abslineinfo, cast_uint(n));
  }
  n = (D->strip) ? 0 : f->sizelocvars;
  dumpInt(D, n);
  for (i = 0; i < n; i++) {
    dumpString(D, f->locvars[i].varname);
    dumpInt(D, f->locvars[i].startpc);
    dumpInt(D, f->locvars[i].endpc);
  }
  n = (D->strip) ? 0 : f->sizeupvalues;
  dumpInt(D, n);
  for (i = 0; i < n; i++)
    dumpString(D, f->upvalues[i].name);
}


static void dumpFunction (DumpState *D, const Proto *f) {
  int saved = D->encrypted;
  dumpByte(D, f->is_encrypted);  /* Diluvium: secure-function marker */
  /* Everything written for this function (code, constant strings,
     source and debug names) is scrambled iff it is secure. Children of
     a secure function are themselves marked secure at parse time, so
     each proto's own flag is authoritative; save/restore across the
     recursion keeps the parent's context for its trailing source dump. */
  D->encrypted = f->is_encrypted;
  dumpInt(D, f->linedefined);
  dumpInt(D, f->lastlinedefined);
  dumpByte(D, f->numparams);
  dumpByte(D, f->flag);
  dumpByte(D, f->maxstacksize);
  dumpCode(D, f);
  dumpConstants(D, f);
  dumpUpvalues(D, f);
  dumpProtos(D, f);
  dumpString(D, D->strip ? NULL : f->source);
  dumpDebug(D, f);
  D->encrypted = saved;
}


#define dumpNumInfo(D, tvar, value)  \
  { tvar i = value; dumpByte(D, sizeof(tvar)); dumpVar(D, i); }


/*
** Diluvium: the dump format's portability rests on these two sizes.
**
** 'dumpInteger', 'dumpSize' and 'dumpInt' are varints, so the body of a
** chunk does not depend on how wide an integer is -- but the header
** records 'sizeof(lua_Integer)' and 'sizeof(lua_Number)' and 'checkHeader'
** refuses a chunk that disagrees. Pinning the types in luaconf.h is what
** makes every Diluvium build agree; this is what says so at compile time,
** so a build flag cannot move them the way LUA_USE_C89 once did.
**
** The negative array size is the C89 spelling of a static assertion:
** '-std=c99' is what this is compiled with, so '_Static_assert' is not
** available. A failure here reads as "size of array is negative", which
** is the sizes below being wrong and nothing else.
*/
#if !defined(DILUVIUM_NUMBERS_UNPINNED)
typedef char diluvium_numbers_are_pinned[
  (sizeof(lua_Integer) == 8 && sizeof(lua_Number) == 8) ? 1 : -1];
#endif


static void dumpHeader (DumpState *D) {
  dumpLiteral(D, LUA_SIGNATURE);
  dumpByte(D, LUAC_VERSION);
  dumpByte(D, LUAC_FORMAT);
  dumpLiteral(D, LUAC_DATA);
  dumpNumInfo(D, int, LUAC_INT);
  dumpNumInfo(D, Instruction, LUAC_INST);
  dumpNumInfo(D, lua_Integer, LUAC_INT);
  dumpNumInfo(D, lua_Number, LUAC_NUM);
}


/*
** dump Lua function as precompiled chunk
*/
int luaU_dump (lua_State *L, const Proto *f, lua_Writer w, void *data,
               int strip) {
  DumpState D;
  D.h = luaH_new(L);  /* aux. table to keep strings already dumped */
  sethvalue2s(L, L->top.p, D.h);  /* anchor it */
  L->top.p++;
  D.L = L;
  D.writer = w;
  D.offset = 0;
  D.data = data;
  D.strip = strip;
  D.status = 0;
  D.nstr = 0;
  D.encrypted = 0;
  /* Diluvium: decide which strings are secure before writing any of
     them; the first write of a shared string is not always the secure
     one, and it is the only copy stored. */
  taintSecureStrings(&D, f, 0);
  dumpHeader(&D);
  dumpByte(&D, f->sizeupvalues);
  dumpFunction(&D, f);
  dumpBlock(&D, NULL, 0);  /* signal end of dump */
  return D.status;
}

