/*
** mp_cursor_fuzz.c
** The codec's two readers on hostile input: the token cursor, and the value
** decoder's appetite for memory.
**
** Why this exists. The cursor (diluvium_mp_open / _read / _skip / _field) is the
** parser the swarm layer uses to read 'system/lifecycle' requests, and those
** requests are written by guest programs. doc/Messaging.md 9 treats a guest as
** untrusted with respect to capability, so a guest that can crash the swarm layer
** or make it misread a request has escaped the thing the capability set exists to
** enforce. Nothing exercised the cursor on hostile input before this file: the
** cross-check in dvs_check.c feeds it bytes the *encoder* produced, which is
** exactly the input that cannot be malformed.
**
** What it asserts, on every input:
**   1. No out-of-bounds read. Enforced by AddressSanitizer, so this must be built
**      with -fsanitize=address to mean anything -- 'make mp_cursor_fuzz' does.
**   2. Termination. Each walk is bounded by a step budget derived from the buffer
**      length; blowing it is a hang, and a hang in 'drain' is a guest stalling the
**      whole swarm.
**   3. The cursor never advances past the end, and every successful read or skip
**      advances it by at least one byte. A zero-advance success is how a parser
**      loops forever.
**   4. 'pending' inside _skip stays bounded by the bytes remaining. This is the
**      one that found a bug: see below.
**
** Determinism: the pseudo-random cases use a fixed-seed LCG written out here
** rather than rand(), so a failure reproduces exactly and the run is identical on
** every platform.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "lua.h"
#include "lauxlib.h"
#include "dmsgpack.h"


static int failures = 0;
static int checks = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s\n", what);
    failures++;
  }
}


/* Print a buffer as hex, so a failure names the exact input. */
static void hexdump (const unsigned char *p, size_t n) {
  size_t i;
  for (i = 0; i < n && i < 48; i++)
    printf("%02x", p[i]);
  if (n > 48) printf("... (%lu bytes)", (unsigned long)n);
}


/*
** Walk a buffer with the cursor, checking the invariants above.
**
** Returns 0 if an invariant was violated, and prints why. A parse *failure* is
** not a violation -- refusing malformed input is the correct outcome and the
** common one. What is checked is that refusing is done safely.
*/
static int walk (const unsigned char *buf, size_t len, const char *what) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  /* Every value costs at least one byte, so a walk cannot legitimately take more
     steps than there are bytes. Plus one so a single-value buffer is not a
     borderline case. */
  size_t budget = len + 2;
  size_t steps = 0;
  const unsigned char *prev;

  diluvium_mp_open(&c, buf, len);
  prev = c.p;
  while (diluvium_mp_read(&c, &t)) {
    steps++;
    if (steps > budget) {
      printf("[FAIL] %s: read did not terminate (%lu steps for %lu bytes): ",
             what, (unsigned long)steps, (unsigned long)len);
      hexdump(buf, len);
      printf("\n");
      return 0;
    }
    if (c.p < prev || c.p > buf + len) {
      printf("[FAIL] %s: cursor left the buffer: ", what);
      hexdump(buf, len);
      printf("\n");
      return 0;
    }
    if (c.p == prev) {
      printf("[FAIL] %s: a successful read advanced nothing, so a caller "
             "looping on _read would hang: ", what);
      hexdump(buf, len);
      printf("\n");
      return 0;
    }
    /* A STR or EXT token points into the caller's buffer; the pointer and length
       must describe bytes that are actually in it. ASan would catch a read, but
       checking the arithmetic states the contract. */
    if ((t.kind == DILUVIUM_MP_STR || t.kind == DILUVIUM_MP_EXT) && t.len > 0) {
      const char *lo = (const char *)buf;
      if (t.p < lo || t.p + t.len > lo + len) {
        printf("[FAIL] %s: a token pointed outside the buffer: ", what);
        hexdump(buf, len);
        printf("\n");
        return 0;
      }
      /* Touch every byte so ASan has something to object to if it is wrong. */
      {
        volatile unsigned char sink = 0;
        size_t i;
        for (i = 0; i < t.len; i++) sink ^= (unsigned char)t.p[i];
        (void)sink;
      }
    }
    prev = c.p;
  }
  return 1;
}


/* The same buffer, walked with _skip instead of _read. */
static int walk_skip (const unsigned char *buf, size_t len, const char *what) {
  diluvium_mp_cursor c;
  size_t budget = len + 2;
  size_t steps = 0;
  const unsigned char *prev;
  diluvium_mp_open(&c, buf, len);
  prev = c.p;
  while (diluvium_mp_skip(&c)) {
    steps++;
    if (steps > budget) {
      printf("[FAIL] %s: skip did not terminate (%lu steps for %lu bytes): ",
             what, (unsigned long)steps, (unsigned long)len);
      hexdump(buf, len);
      printf("\n");
      return 0;
    }
    if (c.p <= prev || c.p > buf + len) {
      printf("[FAIL] %s: skip advanced wrongly (%ld -> %ld of %lu): ", what,
             (long)(prev - buf), (long)(c.p - buf), (unsigned long)len);
      hexdump(buf, len);
      printf("\n");
      return 0;
    }
    prev = c.p;
  }
  return 1;
}


/* And with _field, which is _read plus _skip and the one the swarm calls. */
static int walk_field (const unsigned char *buf, size_t len, const char *what) {
  static const char *keys[] = { "op", "id", "code", "caps", "", "x",
                                "wake_on_message", "instructions" };
  size_t k;
  for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
    diluvium_mp_cursor c;
    diluvium_mp_open(&c, buf, len);
    if (diluvium_mp_field(&c, keys[k])) {
      /* Found: the cursor is on the value, so reading it must be safe. */
      diluvium_mp_token t;
      if (c.p < buf || c.p > buf + len) {
        printf("[FAIL] %s: _field(%s) left the cursor outside the buffer: ",
               what, keys[k]);
        hexdump(buf, len);
        printf("\n");
        return 0;
      }
      diluvium_mp_read(&c, &t);   /* may fail; must not crash */
    }
  }
  return 1;
}


static int walk_all (const unsigned char *buf, size_t len, const char *what) {
  return walk(buf, len, what) && walk_skip(buf, len, what) &&
         walk_field(buf, len, what);
}


/* ------------------------------------------------------- exhaustive prefixes */

/*
** Every possible first byte, alone and then followed by 1..9 bytes of filler.
**
** This is the cheapest complete test of the type dispatch: 256 opcodes times ten
** lengths covers every "the header says N bytes follow and they do not" case for
** every type, which is the whole family of truncation bugs.
*/
static void every_first_byte (void) {
  unsigned char buf[16];
  int t;
  size_t extra;
  int bad = 0;
  for (t = 0; t < 256; t++) {
    for (extra = 0; extra <= 9; extra++) {
      size_t i;
      buf[0] = (unsigned char)t;
      /* 0xff filler, because a length field of all-ones is the largest claim the
         header can make and therefore the most likely to overflow something. */
      for (i = 1; i <= extra; i++) buf[i] = 0xff;
      if (!walk_all(buf, 1 + extra, "every first byte")) { bad = 1; break; }
    }
    if (bad) break;
  }
  ok(!bad, "all 256 opcodes survive being truncated at every length 0..9");
}


/* ------------------------------------------------------- hostile structures */

static void hostile_lengths (void) {
  int bad = 0;

  /* An array claiming 2^32-1 elements with nothing after it. */
  {
    static const unsigned char b[] = { 0xdd, 0xff, 0xff, 0xff, 0xff };
    if (!walk_all(b, sizeof(b), "array32 claiming 4G elements")) bad = 1;
  }
  /* A map claiming 2^32-1 pairs. */
  {
    static const unsigned char b[] = { 0xdf, 0xff, 0xff, 0xff, 0xff };
    if (!walk_all(b, sizeof(b), "map32 claiming 4G pairs")) bad = 1;
  }
  /* str32 claiming 4G bytes with three present. */
  {
    static const unsigned char b[] = { 0xdb, 0xff, 0xff, 0xff, 0xff, 1, 2, 3 };
    if (!walk_all(b, sizeof(b), "str32 claiming 4G bytes")) bad = 1;
  }
  /* ext32 whose length runs past the end. */
  {
    static const unsigned char b[] = { 0xc9, 0xff, 0xff, 0xff, 0xff, 0x42, 1 };
    if (!walk_all(b, sizeof(b), "ext32 past the end")) bad = 1;
  }
  /*
  ** THE ONE THAT FOUND A BUG. Two array32 headers chosen so the running count of
  ** owed values in _skip lands on exactly 2^32.
  **
  ** _skip accumulates 'pending' as a size_t. On a 64-bit size_t this sequence is
  ** harmless: the count reaches 0x100000000 and the walk then fails when the
  ** bytes run out, which is correct. On a 32-bit size_t -- and wasm32 is ILP32,
  ** which this project ships and which 11's dv_layout exists because of -- the
  ** same addition wraps to zero, _skip returns success having consumed ten bytes,
  ** and _field goes on to read whatever follows as a map key.
  **
  ** Kept as a named case rather than folded into the random corpus because the
  ** arithmetic is the point and a reader should see the numbers.
  */
  {
    static const unsigned char b[] = {
      0xdd, 0xff, 0xff, 0xff, 0xff,   /* array32, 0xFFFFFFFF elements */
      0xdd, 0x00, 0x00, 0x00, 0x02,   /* array32, 2 elements          */
      0xa1, 'x'                       /* something to misread         */
    };
    diluvium_mp_cursor c;
    diluvium_mp_open(&c, b, sizeof(b));
    /* The claim: this must be refused. A buffer of twelve bytes cannot satisfy a
       promise of four billion values, and saying so is the only safe answer. */
    if (diluvium_mp_skip(&c)) {
      printf("[FAIL] a 12-byte buffer claiming 4G values was accepted by _skip; "
             "on a 32-bit size_t this is the wrap\n");
      bad = 1;
    }
    /*
    ** And refused *early*. This is the assertion that makes the 32-bit wrap
    ** visible on a 64-bit machine, where the wrap itself cannot happen: a _skip
    ** that decides by counting down four billion owed values consumes the whole
    ** buffer before failing, while one that compares the claim against the bytes
    ** remaining stops at the header. Same verdict, different work -- and the
    ** bounded version is the one whose arithmetic cannot overflow at any width.
    */
    if (c.left == 0) {
      printf("[FAIL] _skip consumed all 12 bytes before refusing a 4G claim, so "
             "it is counting rather than bounding -- the arithmetic that wraps "
             "on a 32-bit size_t\n");
      bad = 1;
    }
    if (!walk_all(b, sizeof(b), "the 2^32 pending wrap")) bad = 1;
  }
  /* The same shape via maps, where the count is doubled before being added. */
  {
    static const unsigned char b[] = {
      0xdf, 0x7f, 0xff, 0xff, 0xff,   /* map32, 0x7FFFFFFF pairs -> 2^32-2 */
      0xdd, 0x00, 0x00, 0x00, 0x02,
      0xa1, 'x'
    };
    diluvium_mp_cursor c;
    diluvium_mp_open(&c, b, sizeof(b));
    if (diluvium_mp_skip(&c)) {
      printf("[FAIL] a 12-byte buffer claiming 2^32-2 map values was accepted\n");
      bad = 1;
    }
    if (!walk_all(b, sizeof(b), "the map doubling wrap")) bad = 1;
  }
  ok(!bad, "an impossible claimed length is refused, not accumulated");
}


static void deep_nesting (void) {
  /*
  ** Twenty thousand nested one-element arrays. A recursive skip meets the C stack
  ** here; the iterative one does not. Depth is unbounded by design (9.1.1 says
  ** delegation depth is unbounded), so this is a real shape and not a stunt.
  */
  size_t n = 20000;
  unsigned char *b = (unsigned char *)malloc(n + 1);
  size_t i;
  int good;
  if (b == NULL) { ok(0, "allocating the deep-nesting buffer"); return; }
  for (i = 0; i < n; i++) b[i] = 0x91;   /* fixarray, 1 element */
  b[n] = 0xc0;                            /* nil, to close it off */
  good = walk_all(b, n + 1, "20000-deep nesting");
  ok(good, "20000 levels of nesting neither crash nor exhaust the stack");
  /* And the same without the closing value, so every level is unsatisfied. */
  if (good)
    good = walk_all(b, n, "20000-deep nesting, unterminated");
  ok(good, "and the same unterminated, which must be refused rather than crash");
  free(b);
}


/* -------------------------------------------------------------- random fuzz */

/* A fixed-seed LCG: deterministic across platforms and runs, so any failure
   reproduces from the printed seed alone. */
static unsigned long rng_state = 0x2545F491UL;

static unsigned int rnd (unsigned int n) {
  rng_state = rng_state * 1103515245UL + 12345UL;
  return (unsigned int)((rng_state >> 16) % n);
}


static void random_bytes (void) {
  unsigned char buf[512];
  int iter;
  int bad = 0;
  size_t tried = 0;
  for (iter = 0; iter < 200000 && !bad; iter++) {
    size_t len = rnd(64) + 1;
    size_t i;
    for (i = 0; i < len; i++) buf[i] = (unsigned char)rnd(256);
    if (!walk_all(buf, len, "random bytes")) bad = 1;
    tried++;
  }
  printf("      (%lu random buffers)\n", (unsigned long)tried);
  ok(!bad, "random bytes never read out of bounds, hang, or stall the cursor");
}


static void structured_fuzz (void) {
  /*
  ** Random bytes are mostly rejected at the first opcode, so they exercise the
  ** dispatch and little else. This builds buffers out of *valid* headers with
  ** random lengths, which is what actually reaches the accumulation and
  ** container-walking paths.
  */
  static const unsigned char heads[] = {
    0x80, 0x90, 0xa0, 0xc0, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf
  };
  unsigned char buf[256];
  int iter;
  int bad = 0;
  size_t tried = 0;
  for (iter = 0; iter < 200000 && !bad; iter++) {
    size_t len = 0;
    size_t want = rnd(40) + 1;
    while (len < want && len < sizeof(buf) - 8) {
      unsigned char h = heads[rnd(sizeof(heads))];
      /* A fixarray/fixmap/fixstr with a random low nibble, or a full header
         followed by a random big-endian length. */
      if (h == 0x80 || h == 0x90 || h == 0xa0)
        buf[len++] = (unsigned char)(h | rnd(16));
      else {
        buf[len++] = h;
        {
          size_t w = rnd(5);   /* 0..4 length bytes, deliberately sometimes wrong */
          size_t j;
          for (j = 0; j < w && len < sizeof(buf); j++)
            buf[len++] = (unsigned char)(rnd(4) == 0 ? 0xff : rnd(256));
        }
      }
    }
    if (!walk_all(buf, len, "structured fuzz")) bad = 1;
    tried++;
  }
  printf("      (%lu structured buffers)\n", (unsigned long)tried);
  ok(!bad, "buffers built from real headers with hostile lengths are safe too");
}


/* ------------------------------------------------ the encoder still agrees */

static void the_happy_path_still_works (void) {
  /*
  ** A fuzzer that made the cursor reject everything would pass every check above,
  ** so this pins the other side down: a well-formed message must still parse.
  ** Hand-encoded rather than taken from the encoder, because this file must not
  ** need a lua_State.
  */
  static const unsigned char b[] = {
    0x82,                                   /* map, 2 pairs      */
    0xa2, 'o', 'p',                         /* "op"              */
    0xa5, 's', 'p', 'a', 'w', 'n',          /* "spawn"           */
    0xa4, 'c', 'a', 'p', 's',               /* "caps"            */
    0x91, 0xa1, 'a'                         /* ["a"]             */
  };
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, b, sizeof(b));
  ok(diluvium_mp_field(&c, "op") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_STR && t.len == 5 &&
     memcmp(t.p, "spawn", 5) == 0, "a well-formed message still parses");
  diluvium_mp_open(&c, b, sizeof(b));
  ok(diluvium_mp_field(&c, "caps") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_ARRAY && t.len == 1,
     "and a field found past an earlier one, so _skip still works");
  diluvium_mp_open(&c, b, sizeof(b));
  ok(!diluvium_mp_field(&c, "absent"), "and an absent field is still absent");
}


/* ------------------------------------------ what a claimed length may cost */

/*
** A counting allocator, so the bound is asserted in bytes rather than guessed from
** RSS. RSS depends on the system allocator's habits and would make this test a
** coin-flip on some platform; the number of bytes Lua asks for does not.
*/
typedef struct { size_t live; size_t peak; } counter;

static void *counting_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  counter *c = (counter *)ud;
  if (nsize == 0) {
    c->live -= osize;
    free(ptr);
    return NULL;
  }
  {
    void *p = realloc(ptr, nsize);
    if (p == NULL) return NULL;
    c->live += nsize;
    if (osize <= c->live) c->live -= osize;
    if (c->live > c->peak) c->peak = c->live;
    return p;
  }
}

static int decode_bomb (lua_State *L) {
  const char *b = (const char *)lua_touserdata(L, 1);
  diluvium_msgpack_decode(L, b, (size_t)lua_tointeger(L, 2));
  return 1;
}

static void a_claimed_length_costs_nothing_until_it_is_read (void) {
  /*
  ** 'dd 08 00 00 00' is an array header claiming 134,217,728 elements, in five
  ** bytes. Sizing a table for the claim allocated 131 MB before reading a single
  ** element -- a 26,000-fold amplification on a path that then failed with
  ** "truncated input". It matters beyond a guest harming itself: queue delivery
  ** decodes on the guest's behalf, and 7.4's store-and-forward means the bytes can
  ** come from another instance, so a five-byte message could make a peer balloon
  ** and 6.2's bounded queues would be bounding the wrong thing.
  */
  static const struct { const char *name; char b[5]; size_t n; } bombs[] = {
    { "array32 claiming 134M elements",  { (char)0xdd, 0x08, 0x00, 0x00, 0x00 }, 5 },
    { "array32 claiming 2^31-1",         { (char)0xdd, 0x7f, (char)0xff, (char)0xff, (char)0xff }, 5 },
    { "map32 claiming 134M pairs",       { (char)0xdf, 0x08, 0x00, 0x00, 0x00 }, 5 },
    { "array16 claiming 65535",          { (char)0xdc, (char)0xff, (char)0xff, 0, 0 }, 3 },
  };
  size_t i;
  int all = 1;
  for (i = 0; i < sizeof(bombs) / sizeof(bombs[0]); i++) {
    counter cnt;
    lua_State *L;
    size_t baseline;
    cnt.live = 0; cnt.peak = 0;
    L = lua_newstate(counting_alloc, &cnt, 0);
    if (L == NULL) { ok(0, "a counted state"); return; }
    baseline = cnt.peak;
    lua_pushcfunction(L, decode_bomb);
    lua_pushlightuserdata(L, (void *)bombs[i].b);
    lua_pushinteger(L, (lua_Integer)bombs[i].n);
    lua_pcall(L, 2, 1, 0);        /* must fail; what matters is the cost */
    /*
    ** A megabyte of headroom over the bare state. The honest bound is "a few times
    ** the input", but a fresh lua_State's own growth is the floor here, so this is
    ** loose on purpose while still being 100x below the 131 MB it used to take.
    */
    printf("      %-34s peak %lu KB over baseline\n", bombs[i].name,
           (unsigned long)((cnt.peak - baseline) / 1024));
    if (cnt.peak > baseline + 1024 * 1024) {
      printf("[FAIL] %s: allocated %lu KB above baseline from %lu input bytes\n",
             bombs[i].name, (unsigned long)((cnt.peak - baseline) / 1024),
             (unsigned long)bombs[i].n);
      all = 0;
    }
    lua_close(L);
  }
  ok(all, "a claimed element count allocates nothing until elements are read");
}


int main (void) {
  printf("=== the msgpack codec on hostile input ===\n");
  a_claimed_length_costs_nothing_until_it_is_read();
  the_happy_path_still_works();
  every_first_byte();
  hostile_lengths();
  deep_nesting();
  random_bytes();
  structured_fuzz();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
