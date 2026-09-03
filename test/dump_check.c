/*
** dump_check.c
** The bytecode header is the platform contract. This checks it.
**
** Diluvium pins lua_Integer and lua_Number (see luaconf.h) so a compiled
** chunk loads on every target rather than only the one that produced it.
** Three things have to hold for that, and each is checked here:
**
**   1. This build's numeric types are the pinned ones. ldump.c asserts
**      that at compile time; asserted again here so a run says so too.
**   2. The header a dump produces is byte-for-byte what the format
**      specifies -- which covers the sizes, the sentinels' byte order,
**      LUAC_VERSION and LUAC_FORMAT together.
**   3. The loader refuses a chunk whose header disagrees, rather than
**      misreading it. A silent misread is the failure worth fearing;
**      a refusal is the format working.
**
** The real cross-load -- one build dumping and a differently configured
** build refusing -- is test/dump_cross_check.sh, which needs two binaries
** and so cannot live in one C file.
**
** Written against the public API plus lundump.h's constants, so it reads
** as a statement about the format rather than about the implementation.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lundump.h"


static int checks = 0, failures = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond) printf("[PASS] %s\n", what);
  else { printf("[FAIL] %s\n", what); failures++; }
}


/*
** The header, byte for byte. Hardcoded rather than rebuilt from the
** macros on purpose: a golden that recomputes itself from the same
** constants it is checking cannot notice them changing.
**
**   00-03  LUA_SIGNATURE               "\x1bLua"
**   04     LUAC_VERSION                0x55, Lua 5.5
**   05     LUAC_FORMAT                 0x46, Diluvium format 3
**   06-11  LUAC_DATA                   catches newline and 8-bit mangling
**   12     sizeof(int)                 4
**   13-16  LUAC_INT   as int           -0x5678, little-endian
**   17     sizeof(Instruction)         4
**   18-21  LUAC_INST  as Instruction   0x12345678, little-endian
**   22     sizeof(lua_Integer)         8   <- the pin
**   23-30  LUAC_INT   as lua_Integer   -0x5678, little-endian
**   31     sizeof(lua_Number)          8   <- the pin
**   32-39  LUAC_NUM   as lua_Number    -370.5, IEEE-754 binary64, LE
**
** Changing any of these is changing what a Diluvium chunk is. If this
** fails, the question is not "update the golden" but "was that meant".
*/
static const unsigned char golden[] = {
  0x1b, 0x4c, 0x75, 0x61, 0x55, 0x46, 0x19, 0x93,
  0x0d, 0x0a, 0x1a, 0x0a, 0x04, 0x88, 0xa9, 0xff,
  0xff, 0x04, 0x78, 0x56, 0x34, 0x12, 0x08, 0x88,
  0xa9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x08,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x77, 0xc0
};

#define HEADERLEN	(sizeof(golden) / sizeof(golden[0]))

/* Offsets into the header the mutation tests reach for. */
#define OFF_INTSIZE	22	/* sizeof(lua_Integer) */
#define OFF_NUMSIZE	31	/* sizeof(lua_Number) */
#define OFF_INTVALUE	23	/* first byte of LUAC_INT as lua_Integer */


/* ====================================================================== */
/* A dump to work from                                                    */
/* ====================================================================== */

typedef struct { char *b; size_t n; } Buf;

static int collect (lua_State *L, const void *p, size_t sz, void *ud) {
  Buf *buf = (Buf *)ud;
  char *grown = (char *)realloc(buf->b, buf->n + sz);
  (void)L;
  if (grown == NULL) return 1;
  memcpy(grown + buf->n, p, sz);
  buf->b = grown;
  buf->n += sz;
  return 0;
}


/* Compile 'src' and dump it. The caller frees 'out->b'. */
static void dumpchunk (lua_State *L, const char *src, Buf *out) {
  out->b = NULL;
  out->n = 0;
  if (luaL_loadstring(L, src) != LUA_OK) {
    printf("[FAIL] cannot compile %s: %s\n", src, lua_tostring(L, -1));
    failures++;
    exit(1);
  }
  if (lua_dump(L, collect, out, 0) != 0) {
    printf("[FAIL] cannot dump %s\n", src);
    failures++;
    exit(1);
  }
  lua_pop(L, 1);
}


/* ====================================================================== */
/* The checks                                                             */
/* ====================================================================== */

static void check_sizes (void) {
  ok(sizeof(lua_Integer) == 8, "lua_Integer is 8 bytes");
  ok(sizeof(lua_Number) == 8, "lua_Number is 8 bytes");
  ok(sizeof(int) == 4, "int is 4 bytes");
}


static void check_header (lua_State *L) {
  Buf d;
  dumpchunk(L, "return 1", &d);
  ok(d.n >= HEADERLEN, "a dump is at least a header long");
  if (d.n >= HEADERLEN && memcmp(d.b, golden, HEADERLEN) != 0) {
    size_t i;
    printf("[FAIL] the header is not what the format says\n");
    printf("       want:");
    for (i = 0; i < HEADERLEN; i++) printf(" %02x", golden[i]);
    printf("\n        got:");
    for (i = 0; i < HEADERLEN; i++) printf(" %02x", (unsigned char)d.b[i]);
    printf("\n");
    for (i = 0; i < HEADERLEN; i++) {
      if ((unsigned char)d.b[i] != golden[i]) {
        printf("       first difference at byte %d\n", (int)i);
        break;
      }
    }
    checks++;
    failures++;
  }
  else ok(1, "the header is byte-for-byte what the format says");
  free(d.b);
}


static void check_roundtrip (lua_State *L) {
  Buf d;
  /* Values chosen to be wrong on a 32-bit-integer build: maxinteger does
     not fit, and the sum stays an integer only where integers are 64-bit. */
  dumpchunk(L, "return math.maxinteger, 3000000000 + 1, 2^0.5", &d);
  ok(luaL_loadbuffer(L, d.b, d.n, "=dumped") == LUA_OK,
     "a chunk this build dumped loads back");
  if (lua_pcall(L, 0, 3, 0) == LUA_OK) {
    ok(lua_tointeger(L, -3) == LUA_MAXINTEGER, "maxinteger survives the trip");
    ok(lua_isinteger(L, -2), "3000000000 + 1 is still an integer");
    ok(lua_tonumber(L, -1) > 1.414 && lua_tonumber(L, -1) < 1.415,
       "a float constant survives the trip");
    lua_pop(L, 3);
  }
  else {
    printf("[FAIL] the round-tripped chunk raised: %s\n", lua_tostring(L, -1));
    checks++; failures++;
    lua_pop(L, 1);
  }
  free(d.b);
}


/*
** A header claiming other numeric types must be refused. This is the
** property that makes the pin safe to rely on: get it wrong and a chunk
** is rejected, never quietly misread.
*/
static void check_refusals (lua_State *L) {
  Buf d;
  size_t i;
  struct { size_t at; unsigned char to; const char *what; } cases[] = {
    { OFF_INTSIZE,  4,    "a chunk claiming a 4-byte lua_Integer is refused" },
    { OFF_NUMSIZE,  4,    "a chunk claiming a 4-byte lua_Number is refused" },
    { OFF_INTVALUE, 0x00, "a chunk whose integer sentinel is byte-swapped "
                          "is refused" },
    { 4,            0x54, "a chunk from another Lua version is refused" },
    { 5,            0x00, "a stock-Lua chunk is refused" }
  };
  dumpchunk(L, "return 1", &d);
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *copy = (char *)malloc(d.n);
    int status;
    memcpy(copy, d.b, d.n);
    copy[cases[i].at] = (char)cases[i].to;
    status = luaL_loadbuffer(L, copy, d.n, "=mutated");
    ok(status != LUA_OK, cases[i].what);
    if (status != LUA_OK) printf("         (%s)\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    free(copy);
  }
  free(d.b);
}


int main (void) {
  lua_State *L = luaL_newstate();
  if (L == NULL) { printf("[FAIL] no state\n"); return 1; }
  luaL_openlibs(L);

  printf("== dump_check: the bytecode header is the platform contract ==\n");
  check_sizes();
  check_header(L);
  check_roundtrip(L);
  check_refusals(L);

  lua_close(L);
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
