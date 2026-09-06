/*
** dregex.c
** The 'regex' guest library. See dregex.h for why it exists and why it is a
** Thompson NFA rather than a backtracker.
**
** The pattern syntax is the RE2/Go subset of PCRE:
**
**   literals            any byte that is not a metacharacter
**   .                   one byte, not '\n' unless (?s)
**   [abc] [^a-z]        a byte class; \d \w \s and [:alpha:] work inside it
**   \d \D \w \W \s \S   digit, word, space, and their complements (ASCII)
**   \b \B               a word boundary, and not one
**   \A \z               the start and the end of the subject
**   ^ $                 the same, or a line boundary under (?m)
**   ( ) (?: )           a capturing group, and a plain one
**   (?<name> )          a named capturing group; (?P<name> ) is the same
**   |                   alternation, preferring the left branch
**   * + ? {n} {n,} {n,m}  greedy repetition; a trailing '?' makes it lazy
**   (?i) (?s) (?m)      ignore case, '.' matches '\n', '^'/'$' per line;
**                       at the start of the pattern only, and they apply to
**                       all of it
**   \n \r \t \f \v \a \e \0 \xHH   the usual byte escapes
**   \<punctuation>      that character, literally
**
** Refused by name, with the reason, rather than silently mis-parsed:
** backreferences (\1), lookahead ((?=, (?!), lookbehind ((?<=, (?<!) and
** Unicode property classes (\p{...}). The first three are not regular and
** cannot be simulated in O(len * program); the fourth is a table this runtime
** does not carry.
**
** The pipeline is the usual three stages, each with its own section below:
** parse the pattern into nodes, compile the nodes into a program, then
** simulate the program over the subject. The program is a byte string rather
** than a C structure, which is what makes a compiled regex snapshot-able
** (dregex.h), and it means the simulator reads a buffer a Lua program could
** have written -- so it bounds-checks every operand it reads, and a forged
** program fails to match rather than reaching memory.
*/

#define dregex_c

#include "lprefix.h"

#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dregex.h"


/* ======================================================================
** Surface
** ====================================================================== */

/* ---- entry points ---------------------------------------------------- */

static int dre_l_compile (lua_State *L);   /* regex.compile(pat [, flags]) */
static int dre_l_escape  (lua_State *L);   /* regex.escape(text)           */
static int dre_l_find    (lua_State *L);   /* re:find(s [, init])          */
static int dre_l_match   (lua_State *L);   /* re:match(s [, init])         */
static int dre_l_gmatch  (lua_State *L);   /* re:gmatch(s [, init])        */
static int dre_l_gsub    (lua_State *L);   /* re:gsub(s, repl [, n])       */
static int dre_l_split   (lua_State *L);   /* re:split(s [, limit])        */

/*
** The module and the methods hold the *same* C functions, which is why
** 'regex.find(re, s)' and 're:find(s)' are one call with one argument order:
** the pattern is always the first argument. That costs one divergence from
** 'string.find', where the subject comes first, and buys a library with a
** single shape to remember.
*/
static const luaL_Reg dre_module[] = {
  {"compile", dre_l_compile},
  {"escape",  dre_l_escape},
  {"find",    dre_l_find},
  {"match",   dre_l_match},
  {"gmatch",  dre_l_gmatch},
  {"gsub",    dre_l_gsub},
  {"split",   dre_l_split},
  {NULL, NULL}
};

static const luaL_Reg dre_methods[] = {
  {"find",   dre_l_find},
  {"match",  dre_l_match},
  {"gmatch", dre_l_gmatch},
  {"gsub",   dre_l_gsub},
  {"split",  dre_l_split},
  {NULL, NULL}
};


/* ---- configurable values --------------------------------------------- */

/*
** Every limit here is a refusal with a message, never a truncation. They exist
** because a pattern is input like any other: 'regex.compile' may be handed a
** string that came off a queue, and the memory the simulator allocates is a
** function of the program size, so an unbounded program is an unbounded
** allocation. The numbers are far above what a pattern a human wrote reaches
** -- an 80-character regex compiles to some 40 instructions.
*/
#define DRE_MAX_PATTERN   1024   /* bytes of pattern text                    */
#define DRE_MAX_NODES      512   /* parse nodes, roughly one per atom        */
#define DRE_MAX_INST       512   /* instructions in a compiled program       */
#define DRE_MAX_CLASS       64   /* distinct byte classes in one program     */
#define DRE_MAX_GROUPS      24   /* capturing groups, the whole match aside  */
#define DRE_MAX_DEPTH       48   /* nesting of groups and repeats            */
#define DRE_MAX_LOOPS       32   /* repetitions whose body can match nothing  */
#define DRE_MAX_REPEAT     255   /* the n and m of {n,m}                     */
#define DRE_MAX_NAME        32   /* bytes in a group name                    */
#define DRE_CACHE_MAX       64   /* compiled patterns kept for reuse         */


/* ---- the program encoding, and the fan-out that reads it -------------- */

/*
** A compiled program is one byte string:
**
**   header    DRE_HDR bytes: magic, version, flags, the group count, the
**             count of hidden slots, and the two counts that say where the
**             rest of the string ends
**   code      'ninst' instructions of DRE_ISZ bytes: op, arg, x, y, where x
**             and y are big-endian instruction indices
**   classes   'nclass' bitmaps of DRE_CLSZ bytes, one bit per byte value
**
** Big-endian and byte-at-a-time on purpose: the string is read back with no
** alignment guarantee and is compared for equality by the compile cache, so
** the encoding cannot depend on how this machine lays out an int.
*/
#define DRE_HDR    12
#define DRE_ISZ     6
#define DRE_CLSZ   32
#define DRE_VERSION 1
#define DRE_NOJ  0xFFFF   /* end of a patch chain; not a valid instruction index */

/*
** The instruction set. 'ninst' is bounded by DRE_MAX_INST, so an index fits
** the 16-bit x and y fields with room to spare.
**
**   DRE_MATCH            the subject matched; captures are this thread's
**   DRE_CHAR   arg=byte  consume exactly this byte
**   DRE_ANY    arg=nl    consume any byte; arg is 1 when '\n' counts too
**   DRE_CLASS  arg=idx   consume a byte in class 'idx'
**   DRE_SPLIT  x, y      two successors, x preferred (this is the whole of
**                        greedy versus lazy: which target comes first)
**   DRE_JMP    x         one successor
**   DRE_SAVE   arg=slot  record the position in capture slot 'arg'
**   DRE_ASSERT arg=kind  a zero-width test, below
**   DRE_LOOP   arg, x, y the back edge of a repetition whose body can match
**                        nothing: go to x when this iteration consumed no
**                        bytes (slot 'arg' holds where it began) and to y
**                        when it did. See 'e_node', which explains why the
**                        rule is "an empty iteration ends the loop" and not
**                        "an empty iteration is refused".
*/
enum {
  DRE_MATCH, DRE_CHAR, DRE_ANY, DRE_CLASS, DRE_SPLIT, DRE_JMP, DRE_SAVE,
  DRE_ASSERT, DRE_LOOP
};

/*
** Zero-width assertions. '^' and '$' compile to BOT/EOT without (?m) and to
** BOL/EOL with it, so the simulator never has to look at a flag: everything
** the flags mean is decided while compiling.
*/
enum {
  DRE_A_BOT,     /* \A   start of subject            */
  DRE_A_EOT,     /* \z   end of subject              */
  DRE_A_BOL,     /* ^    start of subject or of line */
  DRE_A_EOL,     /* $    end of subject or of line   */
  DRE_A_WORDB,   /* \b                               */
  DRE_A_NWORDB   /* \B                               */
};

/* Parse nodes. A sequence is a chain through 'next'; the branches of an
   alternation are a chain through 'alt' off the DRE_N_ALT node's 'child'. */
enum {
  DRE_N_EMPTY, DRE_N_CHAR, DRE_N_ANY, DRE_N_CLASS, DRE_N_ASSERT,
  DRE_N_ALT, DRE_N_REP, DRE_N_GROUP
};

/* Flags, as (?i)(?s)(?m) set them and as the header records them. */
#define DRE_F_ICASE  0x01
#define DRE_F_DOTALL 0x02
#define DRE_F_MULTI  0x04

/*
** Character classes, computed rather than looked up in <ctype.h>: that header
** is locale-dependent, and a runtime whose '\w' changes with an environment
** variable has no determinism story at all. ASCII only; a byte >= 0x80 is in
** none of these and in DRE_C_ANY.
*/
enum {
  DRE_C_DIGIT, DRE_C_LOWER, DRE_C_UPPER, DRE_C_ALPHA, DRE_C_ALNUM,
  DRE_C_SPACE, DRE_C_WORD, DRE_C_PUNCT, DRE_C_CNTRL, DRE_C_XDIGIT,
  DRE_C_BLANK, DRE_C_PRINT, DRE_C_GRAPH, DRE_C_ANY
};

/* '\d' and friends, outside a class and inside one alike: the letter, the
   class it names, and whether it is the complement. */
static const struct { char ch; unsigned char cls; unsigned char neg; }
DRE_ESCAPE_CLASS[] = {
  {'d', DRE_C_DIGIT, 0}, {'D', DRE_C_DIGIT, 1},
  {'w', DRE_C_WORD,  0}, {'W', DRE_C_WORD,  1},
  {'s', DRE_C_SPACE, 0}, {'S', DRE_C_SPACE, 1},
  {0, 0, 0}
};

/* '[:name:]', POSIX's spelling, valid only inside a class. */
static const struct { const char *name; unsigned char cls; } DRE_POSIX_CLASS[] = {
  {"alpha", DRE_C_ALPHA}, {"digit", DRE_C_DIGIT}, {"alnum", DRE_C_ALNUM},
  {"space", DRE_C_SPACE}, {"upper", DRE_C_UPPER}, {"lower", DRE_C_LOWER},
  {"punct", DRE_C_PUNCT}, {"xdigit", DRE_C_XDIGIT}, {"cntrl", DRE_C_CNTRL},
  {"print", DRE_C_PRINT}, {"graph", DRE_C_GRAPH}, {"blank", DRE_C_BLANK},
  {"word", DRE_C_WORD},
  {NULL, 0}
};

/* '\n' and the rest: the letter and the byte it denotes. */
static const struct { char ch; unsigned char byte; } DRE_ESCAPE_BYTE[] = {
  {'n', '\n'}, {'r', '\r'}, {'t', '\t'}, {'f', '\f'}, {'v', '\v'},
  {'a', '\a'}, {'e', 0x1B}, {'0', 0x00},
  {0, 0}
};

/*
** The constructs this engine will not have, and what to say about each. They
** are listed here rather than refused where they are met, so that "what is not
** supported" is one table a reader can check against the syntax summary above
** instead of a search through the parser.
*/
#define DRE_WHY_NOBACKTRACK \
  "; this engine is linear-time and does not backtrack (see dregex.h)"

static const char DRE_MSG_BACKREF[] =
  "backreferences are not supported" DRE_WHY_NOBACKTRACK;
static const char DRE_MSG_LOOKAHEAD[] =
  "lookahead is not supported" DRE_WHY_NOBACKTRACK;
static const char DRE_MSG_LOOKBEHIND[] =
  "lookbehind is not supported" DRE_WHY_NOBACKTRACK;
static const char DRE_MSG_UNIPROP[] =
  "\\p{...} is not supported: this engine matches bytes, and carries no "
  "Unicode property tables";


/* ======================================================================
** depth: the parser -- pattern text to nodes
** ====================================================================== */

typedef struct dre_node {
  unsigned char type;
  unsigned char greedy;   /* DRE_N_REP */
  short group;            /* DRE_N_GROUP: 1..ngroups, or -1 when plain */
  int a;                  /* CHAR: the byte; CLASS: index; ASSERT: kind;
                             ANY: 1 when '\n' counts */
  int min, max;           /* DRE_N_REP; max < 0 is unbounded */
  int child;              /* GROUP, REP: the sequence inside; ALT: branch one */
  int next;               /* the next piece of this sequence */
  int alt;                /* the next branch of the enclosing alternation */
} dre_node;

typedef struct dre_parser {
  const char *p, *start, *end;
  unsigned flags;
  int ngroups;            /* capturing groups seen; group 0 is the whole match */
  int seenatom;           /* has anything been parsed? ((?i) may not follow) */
  dre_node *nodes;
  int nnodes;
  unsigned char *classes; /* nclass * DRE_CLSZ */
  int nclass;
  char (*names)[DRE_MAX_NAME];   /* names[g], "" when the group is unnamed */
  char err[192];
} dre_parser;


static int dre_perr (dre_parser *P, const char *msg) {
  if (P->err[0] == '\0')
    snprintf(P->err, sizeof(P->err), "%s (at offset %d)",
             msg, (int)(P->p - P->start));
  return -1;
}


/* ---- byte classes ---------------------------------------------------- */

/* ASCII and computed, never <ctype.h>: see the enum's comment in the surface. */
static int dre_isclass (int kind, int c) {
  switch (kind) {
    case DRE_C_DIGIT:  return c >= '0' && c <= '9';
    case DRE_C_LOWER:  return c >= 'a' && c <= 'z';
    case DRE_C_UPPER:  return c >= 'A' && c <= 'Z';
    case DRE_C_ALPHA:  return dre_isclass(DRE_C_LOWER, c) ||
                              dre_isclass(DRE_C_UPPER, c);
    case DRE_C_ALNUM:  return dre_isclass(DRE_C_ALPHA, c) ||
                              dre_isclass(DRE_C_DIGIT, c);
    case DRE_C_SPACE:  return c == ' ' || (c >= '\t' && c <= '\r');
    case DRE_C_WORD:   return dre_isclass(DRE_C_ALNUM, c) || c == '_';
    case DRE_C_PUNCT:  return c >= 0x21 && c <= 0x7E &&
                              !dre_isclass(DRE_C_ALNUM, c);
    case DRE_C_CNTRL:  return c < 0x20 || c == 0x7F;
    case DRE_C_XDIGIT: return dre_isclass(DRE_C_DIGIT, c) ||
                              (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    case DRE_C_BLANK:  return c == ' ' || c == '\t';
    case DRE_C_PRINT:  return c >= 0x20 && c < 0x7F;
    case DRE_C_GRAPH:  return c > 0x20 && c < 0x7F;
    case DRE_C_ANY:    return 1;
  }
  return 0;
}

static void dre_bset (unsigned char *b, int c) {
  b[(c >> 3) & (DRE_CLSZ - 1)] |= (unsigned char)(1u << (c & 7));
}

static int dre_bget (const unsigned char *b, int c) {
  return (b[(c >> 3) & (DRE_CLSZ - 1)] >> (c & 7)) & 1;
}

/* Add every case counterpart already in the set, so (?i) needs no flag at
   run time. Applied before negation, so [^a] under (?i) refuses 'A' too. */
static void dre_bfold (unsigned char *b) {
  int c;
  for (c = 'a'; c <= 'z'; c++) {
    if (dre_bget(b, c)) dre_bset(b, c - 'a' + 'A');
    if (dre_bget(b, c - 'a' + 'A')) dre_bset(b, c);
  }
}

/* Classes are deduplicated by content: '[a-z]+[a-z]*' carries one bitmap. */
static int dre_newclass (dre_parser *P, const unsigned char *bits) {
  int i;
  for (i = 0; i < P->nclass; i++)
    if (memcmp(P->classes + i * DRE_CLSZ, bits, DRE_CLSZ) == 0)
      return i;
  if (P->nclass >= DRE_MAX_CLASS)
    return dre_perr(P, "too many character classes in one pattern");
  memcpy(P->classes + P->nclass * DRE_CLSZ, bits, DRE_CLSZ);
  return P->nclass++;
}

/* ---- nodes ----------------------------------------------------------- */

static int dre_newnode (dre_parser *P, int type) {
  dre_node *n;
  if (P->nnodes >= DRE_MAX_NODES)
    return dre_perr(P, "pattern too complex: too many pieces");
  n = &P->nodes[P->nnodes];
  n->type = (unsigned char)type;
  n->greedy = 1;
  n->group = -1;
  n->a = 0;
  n->min = n->max = 0;
  n->child = n->next = n->alt = -1;
  return P->nnodes++;
}

/* One literal byte. Under (?i) a cased letter becomes a two-member class,
   which is the whole implementation of case-insensitive literals. */
static int dre_charnode (dre_parser *P, int byte) {
  int n;
  if ((P->flags & DRE_F_ICASE) && dre_isclass(DRE_C_ALPHA, byte)) {
    unsigned char bits[DRE_CLSZ];
    int idx;
    memset(bits, 0, DRE_CLSZ);
    dre_bset(bits, byte);
    dre_bfold(bits);
    idx = dre_newclass(P, bits);
    if (idx < 0) return -1;
    n = dre_newnode(P, DRE_N_CLASS);
    if (n < 0) return -1;
    P->nodes[n].a = idx;
    return n;
  }
  n = dre_newnode(P, DRE_N_CHAR);
  if (n < 0) return -1;
  P->nodes[n].a = byte;
  return n;
}

static int dre_assertnode (dre_parser *P, int kind) {
  int n = dre_newnode(P, DRE_N_ASSERT);
  if (n < 0) return -1;
  P->nodes[n].a = kind;
  return n;
}


/* ---- escapes --------------------------------------------------------- */

static int dre_hexval (int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/*
** One escape, in the two places escapes appear. On entry 'P->p' is on the
** backslash. Returns the byte it denotes (0..255), or DRE_ESC_CLASS after
** filling 'bits' with a whole class, or DRE_ESC_ASSERT with '*kind' set, or
** -1 with the error recorded. 'bits' and 'kind' may be NULL where that form
** is not accepted -- inside a class, '\b' would be the backspace character in
** Perl and a word boundary here, so it is refused rather than guessed at.
*/
#define DRE_ESC_CLASS   (-2)
#define DRE_ESC_ASSERT  (-3)

static int dre_escape (dre_parser *P, unsigned char *bits, int *kind) {
  int c, i;
  P->p++;                                     /* the backslash */
  if (P->p >= P->end)
    return dre_perr(P, "pattern ends with a lone '\\'");
  c = (unsigned char)*P->p;
  for (i = 0; DRE_ESCAPE_CLASS[i].ch != 0; i++) {
    if (DRE_ESCAPE_CLASS[i].ch == c) {
      int j;
      P->p++;
      if (bits == NULL)                       /* not a place a class fits */
        return DRE_ESC_CLASS;
      for (j = 0; j < 256; j++)
        if (dre_isclass(DRE_ESCAPE_CLASS[i].cls, j) != DRE_ESCAPE_CLASS[i].neg)
          dre_bset(bits, j);
      return DRE_ESC_CLASS;
    }
  }
  switch (c) {
    case 'b': case 'B':
      if (kind == NULL)
        return dre_perr(P, "'\\b' inside a character class is ambiguous: it is "
                           "a word boundary here and a backspace in Perl -- "
                           "write '\\x08' for the character");
      P->p++;
      *kind = (c == 'b') ? DRE_A_WORDB : DRE_A_NWORDB;
      return DRE_ESC_ASSERT;
    case 'A': case 'z':
      if (kind == NULL)
        return dre_perr(P, "an anchor has no meaning inside a character class");
      P->p++;
      *kind = (c == 'A') ? DRE_A_BOT : DRE_A_EOT;
      return DRE_ESC_ASSERT;
    case 'Z':
      return dre_perr(P, "'\\Z' is not supported: write '\\z' for the end of "
                         "the subject, or '\\n?\\z' for Perl's meaning");
    case 'p': case 'P':
      return dre_perr(P, DRE_MSG_UNIPROP);
    case 'k': case 'g':
      return dre_perr(P, DRE_MSG_BACKREF);
    case 'Q': case 'E':
      return dre_perr(P, "'\\Q...\\E' is not supported: quote the text with "
                         "regex.escape instead");
    case 'x': {
      int hi, lo;
      P->p++;
      if (P->p + 1 >= P->end ||
          (hi = dre_hexval((unsigned char)P->p[0])) < 0 ||
          (lo = dre_hexval((unsigned char)P->p[1])) < 0)
        return dre_perr(P, "'\\x' needs exactly two hexadecimal digits");
      P->p += 2;
      return hi * 16 + lo;
    }
    default: break;
  }
  if (c >= '1' && c <= '9')
    return dre_perr(P, DRE_MSG_BACKREF);
  for (i = 0; DRE_ESCAPE_BYTE[i].ch != 0; i++) {
    if (DRE_ESCAPE_BYTE[i].ch == c) {
      P->p++;
      return DRE_ESCAPE_BYTE[i].byte;
    }
  }
  if (dre_isclass(DRE_C_ALNUM, c)) {
    char msg[64];
    snprintf(msg, sizeof(msg), "unknown escape '\\%c'", c);
    return dre_perr(P, msg);
  }
  P->p++;
  return c;                                   /* '\.' and friends: literal */
}


/* ---- character classes, '[...]' -------------------------------------- */

/* On entry 'P->p' is just past the '['. Returns a class index. */
static int dre_parseclass (dre_parser *P) {
  unsigned char bits[DRE_CLSZ];
  int neg = 0, first = 1;
  memset(bits, 0, DRE_CLSZ);
  if (P->p < P->end && *P->p == '^') { neg = 1; P->p++; }
  while (P->p < P->end && (*P->p != ']' || first)) {
    int lo;
    first = 0;
    if (*P->p == '[' && P->p + 1 < P->end && P->p[1] == ':') {
      const char *q = P->p + 2;
      int i;
      while (q < P->end && *q != ':') q++;
      if (q + 1 >= P->end || q[1] != ']')
        return dre_perr(P, "unfinished '[:name:]' class");
      for (i = 0; DRE_POSIX_CLASS[i].name != NULL; i++) {
        size_t n = strlen(DRE_POSIX_CLASS[i].name);
        if (n == (size_t)(q - (P->p + 2)) &&
            memcmp(P->p + 2, DRE_POSIX_CLASS[i].name, n) == 0) {
          int c;
          for (c = 0; c < 256; c++)
            if (dre_isclass(DRE_POSIX_CLASS[i].cls, c)) dre_bset(bits, c);
          break;
        }
      }
      if (DRE_POSIX_CLASS[i].name == NULL)
        return dre_perr(P, "unknown '[:name:]' class");
      P->p = q + 2;
      continue;
    }
    if (*P->p == '\\') {
      lo = dre_escape(P, bits, NULL);
      if (lo == -1) return -1;
      if (lo == DRE_ESC_CLASS) continue;      /* '\d' put its own bits in */
    }
    else
      lo = (unsigned char)*P->p++;
    if (P->p + 1 < P->end && *P->p == '-' && P->p[1] != ']') {
      int hi;
      P->p++;                                 /* the '-' */
      if (*P->p == '\\') {
        hi = dre_escape(P, NULL, NULL);
        if (hi < 0)
          return hi == -1 ? -1
                 : dre_perr(P, "a character class cannot end a range");
      }
      else
        hi = (unsigned char)*P->p++;
      if (hi < lo)
        return dre_perr(P, "reversed range in a character class");
      for (; lo <= hi; lo++) dre_bset(bits, lo);
    }
    else
      dre_bset(bits, lo);
  }
  if (P->p >= P->end)
    return dre_perr(P, "unfinished character class");
  P->p++;                                     /* the ']' */
  if (P->flags & DRE_F_ICASE) dre_bfold(bits);
  if (neg) {
    int i;
    for (i = 0; i < DRE_CLSZ; i++) bits[i] = (unsigned char)~bits[i];
  }
  return dre_newclass(P, bits);
}


/* ---- the grammar ----------------------------------------------------- */

static int dre_alt (dre_parser *P, int depth);

/* '{' starts a repetition only when a full '{n}', '{n,}' or '{n,m}' follows.
   Otherwise it is an ordinary character, which is what a pattern matching a
   brace expects and what every engine that is not RE2 does. */
static int dre_isbound (dre_parser *P) {
  const char *q = P->p + 1;
  if (q >= P->end || !dre_isclass(DRE_C_DIGIT, (unsigned char)*q)) return 0;
  while (q < P->end && dre_isclass(DRE_C_DIGIT, (unsigned char)*q)) q++;
  if (q < P->end && *q == ',') {
    q++;
    while (q < P->end && dre_isclass(DRE_C_DIGIT, (unsigned char)*q)) q++;
  }
  return q < P->end && *q == '}';
}

static int dre_bound (dre_parser *P, int *min, int *max) {
  int v = 0;
  P->p++;                                     /* the '{' */
  while (dre_isclass(DRE_C_DIGIT, (unsigned char)*P->p)) {
    v = v * 10 + (*P->p++ - '0');
    if (v > DRE_MAX_REPEAT)
      return dre_perr(P, "a repetition count above 255");
  }
  *min = v;
  if (*P->p == ',') {
    P->p++;
    if (*P->p == '}')
      *max = -1;                              /* '{n,}' is unbounded */
    else {
      v = 0;
      while (dre_isclass(DRE_C_DIGIT, (unsigned char)*P->p)) {
        v = v * 10 + (*P->p++ - '0');
        if (v > DRE_MAX_REPEAT)
          return dre_perr(P, "a repetition count above 255");
      }
      *max = v;
    }
  }
  else
    *max = *min;
  P->p++;                                     /* the '}' */
  if (*max >= 0 && *max < *min)
    return dre_perr(P, "repetition bounds are the wrong way round");
  return 0;
}

/* '(?ims)', which this accepts only before anything else has been parsed:
   scoped flags would make '(?i)' mean one thing at the top of a pattern and
   another inside a group, and one rule that is easy to state beats two. */
static int dre_flaggroup (dre_parser *P) {
  unsigned add = 0;
  while (P->p < P->end && *P->p != ')') {
    switch (*P->p) {
      case 'i': add |= DRE_F_ICASE; break;
      case 's': add |= DRE_F_DOTALL; break;
      case 'm': add |= DRE_F_MULTI; break;
      case '-':
        return dre_perr(P, "turning a flag off is not supported: a flag "
                           "applies to the whole pattern");
      default:
        return dre_perr(P, "unknown flag: the flags are 'i', 's' and 'm'");
    }
    P->p++;
  }
  if (P->p >= P->end)
    return dre_perr(P, "unfinished '(?'");
  P->p++;                                     /* the ')' */
  if (P->seenatom)
    return dre_perr(P, "flags must come first: '(?i)' and its kin apply to the "
                       "whole pattern and are only accepted at its start");
  P->flags |= add;
  return dre_newnode(P, DRE_N_EMPTY);
}

static int dre_group (dre_parser *P, int depth) {
  int g = -1, body, n, capturing = 1;
  const char *name = NULL;
  size_t namelen = 0;
  P->p++;                                     /* the '(' */
  if (P->p < P->end && *P->p == '?') {
    const char *q = P->p + 1;
    if (q >= P->end)
      return dre_perr(P, "unfinished '(?'");
    if (*q == ':') {
      capturing = 0;
      P->p = q + 1;
    }
    else if (*q == '=' || *q == '!')
      return dre_perr(P, DRE_MSG_LOOKAHEAD);
    else if (*q == '#')
      return dre_perr(P, "'(?#...)' comments are not supported");
    else if (*q == '<' || (*q == 'P' && q + 1 < P->end && q[1] == '<')) {
      if (*q == 'P') q++;                     /* '(?P<name>' is Python's */
      if (q + 1 < P->end && (q[1] == '=' || q[1] == '!'))
        return dre_perr(P, DRE_MSG_LOOKBEHIND);
      q++;                                    /* past the '<' */
      name = q;
      while (q < P->end && *q != '>') {
        if (!dre_isclass(DRE_C_WORD, (unsigned char)*q))
          return dre_perr(P, "a group name may hold only letters, digits "
                              "and '_'");
        q++;
      }
      if (q >= P->end)
        return dre_perr(P, "unfinished group name");
      namelen = (size_t)(q - name);
      if (namelen == 0)
        return dre_perr(P, "an empty group name");
      if (namelen >= DRE_MAX_NAME)
        return dre_perr(P, "a group name of more than 31 bytes");
      P->p = q + 1;                           /* past the '>' */
    }
    else if (*q == 'P')
      return dre_perr(P, DRE_MSG_BACKREF);    /* '(?P=name)' */
    else {
      P->p = q;
      return dre_flaggroup(P);                /* '(?ims)' */
    }
  }
  if (capturing) {
    if (P->ngroups >= DRE_MAX_GROUPS)
      return dre_perr(P, "too many capturing groups");
    g = ++P->ngroups;                         /* numbered where it opens */
    if (name != NULL) {
      memcpy(P->names[g], name, namelen);
      P->names[g][namelen] = '\0';
    }
  }
  body = dre_alt(P, depth + 1);
  if (body < 0) return -1;
  if (P->p >= P->end || *P->p != ')')
    return dre_perr(P, "missing ')'");
  P->p++;
  n = dre_newnode(P, DRE_N_GROUP);
  if (n < 0) return -1;
  P->nodes[n].child = body;
  P->nodes[n].group = (short)g;
  return n;
}

static int dre_atom (dre_parser *P, int depth) {
  int n;
  if (depth > DRE_MAX_DEPTH)
    return dre_perr(P, "pattern nests too deeply");
  switch (*P->p) {
    case '(':
      return dre_group(P, depth);
    case '[': {
      int idx;
      P->p++;
      idx = dre_parseclass(P);
      if (idx < 0) return -1;
      n = dre_newnode(P, DRE_N_CLASS);
      if (n < 0) return -1;
      P->nodes[n].a = idx;
      return n;
    }
    case '.':
      P->p++;
      n = dre_newnode(P, DRE_N_ANY);
      if (n < 0) return -1;
      P->nodes[n].a = (P->flags & DRE_F_DOTALL) ? 1 : 0;
      return n;
    case '^':
      P->p++;
      return dre_assertnode(P, (P->flags & DRE_F_MULTI) ? DRE_A_BOL
                                                        : DRE_A_BOT);
    case '$':
      P->p++;
      return dre_assertnode(P, (P->flags & DRE_F_MULTI) ? DRE_A_EOL
                                                        : DRE_A_EOT);
    case '*': case '+': case '?':
      return dre_perr(P, "nothing to repeat");
    case ']': case '}':
      P->p++;
      return dre_charnode(P, (unsigned char)P->p[-1]);
    case '\\': {
      unsigned char bits[DRE_CLSZ];
      int kind = -1, r;
      memset(bits, 0, DRE_CLSZ);
      r = dre_escape(P, bits, &kind);
      if (r == -1) return -1;
      if (r == DRE_ESC_ASSERT)
        return dre_assertnode(P, kind);
      if (r == DRE_ESC_CLASS) {
        int idx;
        if (P->flags & DRE_F_ICASE) dre_bfold(bits);
        idx = dre_newclass(P, bits);
        if (idx < 0) return -1;
        n = dre_newnode(P, DRE_N_CLASS);
        if (n < 0) return -1;
        P->nodes[n].a = idx;
        return n;
      }
      return dre_charnode(P, r);
    }
    default:
      P->p++;
      return dre_charnode(P, (unsigned char)P->p[-1]);
  }
}

static int dre_piece (dre_parser *P, int depth) {
  int atom, min, max, greedy = 1, rep;
  atom = dre_atom(P, depth);
  if (atom < 0) return -1;
  P->seenatom = 1;
  if (P->p >= P->end) return atom;
  if (*P->p == '*') { min = 0; max = -1; P->p++; }
  else if (*P->p == '+') { min = 1; max = -1; P->p++; }
  else if (*P->p == '?') { min = 0; max = 1; P->p++; }
  else if (*P->p == '{' && dre_isbound(P)) {
    if (dre_bound(P, &min, &max) < 0) return -1;
  }
  else
    return atom;
  if (P->p < P->end && *P->p == '?') {
    greedy = 0;
    P->p++;
  }
  else if (P->p < P->end && *P->p == '+')
    return dre_perr(P, "possessive quantifiers are not supported: they exist "
                       "to tame backtracking, and there is none here");
  if (P->p < P->end &&
      (*P->p == '*' || *P->p == '+' || *P->p == '?' ||
       (*P->p == '{' && dre_isbound(P))))
    return dre_perr(P, "one quantifier applies to one piece: group the piece "
                       "as '(?:...)' to repeat it again");
  rep = dre_newnode(P, DRE_N_REP);
  if (rep < 0) return -1;
  P->nodes[rep].child = atom;
  P->nodes[rep].min = min;
  P->nodes[rep].max = max;
  P->nodes[rep].greedy = (unsigned char)greedy;
  return rep;
}

static int dre_concat (dre_parser *P, int depth) {
  int head = -1, tail = -1;
  while (P->p < P->end && *P->p != '|' && *P->p != ')') {
    int n = dre_piece(P, depth);
    if (n < 0) return -1;
    if (head < 0) head = n;
    else P->nodes[tail].next = n;
    tail = n;
  }
  if (head < 0)
    head = dre_newnode(P, DRE_N_EMPTY);       /* 'a|' and '()' are legal */
  return head;
}

static int dre_alt (dre_parser *P, int depth) {
  int first, last, node;
  if (depth > DRE_MAX_DEPTH)
    return dre_perr(P, "pattern nests too deeply");
  first = dre_concat(P, depth);
  if (first < 0) return -1;
  if (P->p >= P->end || *P->p != '|')
    return first;
  node = dre_newnode(P, DRE_N_ALT);
  if (node < 0) return -1;
  P->nodes[node].child = first;
  last = first;
  while (P->p < P->end && *P->p == '|') {
    int b;
    P->p++;
    b = dre_concat(P, depth);
    if (b < 0) return -1;
    P->nodes[last].alt = b;
    last = b;
  }
  return node;
}


/* ======================================================================
** depth: the compiler -- nodes to a program
** ====================================================================== */

/*
** Thompson's construction, emitted straight into the instruction buffer.
** Forward branches are patched through the instruction stream itself rather
** than through a side table: an unfinished operand holds the index of the
** previous instruction waiting on the same target, DRE_NOJ ends the chain,
** and 'e_patchchain' walks it once the target is known. That is 'lcode.c''s
** jump lists, and it is what keeps an alternation of a hundred branches from
** needing a hundred slots of scratch.
*/

typedef struct dre_emit {
  unsigned char *code;
  int n;                  /* instructions emitted */
  int max;
  int over;               /* the program outgrew DRE_MAX_INST */
  int nhidden;            /* slots taken by empty-loop progress checks */
  int base;               /* the first hidden slot: 2 * (ngroups + 1) */
} dre_emit;

#define DRE_X 0
#define DRE_Y 1

static int e_inst (dre_emit *E, int op, int arg, int x, int y) {
  unsigned char *q;
  if (E->n >= E->max) {
    /* Refused later, by 'over'. Until then every instruction past the end
       reports the same out-of-range index, which 'e_patch' ignores and
       'e_operand' reads as the end of a chain -- so an overflow inside an
       alternation cannot build a patch chain that points at itself, which
       is a hang and not a refusal. */
    E->over = 1;
    return E->max;
  }
  q = E->code + E->n * DRE_ISZ;
  q[0] = (unsigned char)op;
  q[1] = (unsigned char)arg;
  q[2] = (unsigned char)((x >> 8) & 0xFF);
  q[3] = (unsigned char)(x & 0xFF);
  q[4] = (unsigned char)((y >> 8) & 0xFF);
  q[5] = (unsigned char)(y & 0xFF);
  return E->n++;
}

static void e_patch (dre_emit *E, int pc, int which, int target) {
  unsigned char *q;
  if (pc < 0 || pc >= E->max) return;
  q = E->code + pc * DRE_ISZ + (which == DRE_X ? 2 : 4);
  q[0] = (unsigned char)((target >> 8) & 0xFF);
  q[1] = (unsigned char)(target & 0xFF);
}

static int e_operand (dre_emit *E, int pc, int which) {
  const unsigned char *q;
  if (pc < 0 || pc >= E->max) return DRE_NOJ;
  q = E->code + pc * DRE_ISZ + (which == DRE_X ? 2 : 4);
  return (q[0] << 8) | q[1];
}

/* Add 'pc' to the chain of instructions still waiting for a target. */
static void e_link (dre_emit *E, int *chain, int pc, int which) {
  e_patch(E, pc, which, *chain < 0 ? DRE_NOJ : *chain);
  *chain = pc;
}

static void e_patchchain (dre_emit *E, int chain, int which, int target) {
  while (chain >= 0) {
    int nxt = e_operand(E, chain, which);
    e_patch(E, chain, which, target);
    chain = (nxt == DRE_NOJ) ? -1 : nxt;
  }
}


static void e_seq (dre_emit *E, dre_parser *P, int head);

/*
** Can this match the empty string? Only repetitions whose body can need the
** progress check below, and most bodies cannot, so this is what keeps the
** ordinary '(\\w+)*' free of the extra slot and the extra instruction.
*/
static int dre_nullable (dre_parser *P, int head) {
  int n;
  for (n = head; n >= 0; n = P->nodes[n].next) {
    dre_node *nd = &P->nodes[n];
    switch (nd->type) {
      case DRE_N_EMPTY: case DRE_N_ASSERT:
        break;                                /* a sequence of these is empty */
      case DRE_N_CHAR: case DRE_N_ANY: case DRE_N_CLASS:
        return 0;
      case DRE_N_GROUP:
        if (!dre_nullable(P, nd->child)) return 0;
        break;
      case DRE_N_REP:
        if (nd->min > 0 && !dre_nullable(P, nd->child)) return 0;
        break;
      case DRE_N_ALT: {
        int b, any = 0;
        for (b = nd->child; b >= 0; b = P->nodes[b].alt)
          if (dre_nullable(P, b)) { any = 1; break; }
        if (!any) return 0;
        break;
      }
      default:
        return 0;
    }
  }
  return 1;
}

static void e_node (dre_emit *E, dre_parser *P, int ni) {
  dre_node *nd = &P->nodes[ni];
  switch (nd->type) {
    case DRE_N_EMPTY:
      break;
    case DRE_N_CHAR:
      e_inst(E, DRE_CHAR, nd->a, 0, 0);
      break;
    case DRE_N_ANY:
      e_inst(E, DRE_ANY, nd->a, 0, 0);
      break;
    case DRE_N_CLASS:
      e_inst(E, DRE_CLASS, nd->a, 0, 0);
      break;
    case DRE_N_ASSERT:
      e_inst(E, DRE_ASSERT, nd->a, 0, 0);
      break;
    case DRE_N_GROUP:
      if (nd->group > 0) {
        e_inst(E, DRE_SAVE, nd->group * 2, 0, 0);
        e_seq(E, P, nd->child);
        e_inst(E, DRE_SAVE, nd->group * 2 + 1, 0, 0);
      }
      else
        e_seq(E, P, nd->child);
      break;
    case DRE_N_ALT: {
      int jchain = -1;                        /* the JMPs that leave a branch */
      int b = nd->child;
      while (b >= 0) {
        int nextb = P->nodes[b].alt;
        if (nextb < 0)
          e_seq(E, P, b);                     /* the last branch falls through */
        else {
          int sp = e_inst(E, DRE_SPLIT, 0, 0, 0);
          e_patch(E, sp, DRE_X, E->n);        /* prefer the left branch */
          e_seq(E, P, b);
          e_link(E, &jchain, e_inst(E, DRE_JMP, 0, 0, 0), DRE_X);
          e_patch(E, sp, DRE_Y, E->n);
        }
        b = nextb;
      }
      e_patchchain(E, jchain, DRE_X, E->n);
      break;
    }
    case DRE_N_REP: {
      /* Greedy and lazy differ in one thing only: which of a SPLIT's two
         targets is the body and which is the exit. */
      int body = nd->greedy ? DRE_X : DRE_Y;
      int exit = nd->greedy ? DRE_Y : DRE_X;
      int i;
      int slot = -1;
      if (nd->max == 0) break;                /* '{0}' matches the empty string */
      if (nd->max < 0) {                      /* '*', '+', '{n,}' */
        /*
        ** A body that can match nothing needs a back edge that knows whether
        ** this iteration consumed anything, because "keep looping" and "stop"
        ** are otherwise the same state and the closure would cut the second
        ** one. The rule implemented is Perl's, and it is the rule rather than
        ** the simpler "refuse an empty iteration" because the two differ where
        ** it is visible: '((?:\\s)*)*' over "  " leaves group 1 empty in Perl,
        ** Python and here, since the empty iteration happens and *then* ends
        ** the loop. Refusing it outright would leave group 1 holding "  ".
        */
        if (dre_nullable(P, nd->child) && E->nhidden < DRE_MAX_LOOPS)
          slot = E->base + E->nhidden++;
        if (nd->min == 0) {                   /* L: split body,exit; body; -> L */
          int top = E->n;
          int sp = e_inst(E, DRE_SPLIT, 0, 0, 0);
          e_patch(E, sp, body, E->n);
          if (slot >= 0) e_inst(E, DRE_SAVE, slot, 0, 0);
          e_seq(E, P, nd->child);
          if (slot >= 0) {
            int lp = e_inst(E, DRE_LOOP, slot, 0, top);
            e_patch(E, sp, exit, E->n);
            e_patch(E, lp, DRE_X, E->n);      /* no progress: leave the loop */
          }
          else {
            e_patch(E, e_inst(E, DRE_JMP, 0, 0, 0), DRE_X, top);
            e_patch(E, sp, exit, E->n);
          }
        }
        else {                                /* n-1 copies, then body; split */
          int top, lp = -1;
          for (i = 0; i < nd->min - 1; i++) e_seq(E, P, nd->child);
          top = E->n;
          if (slot >= 0) e_inst(E, DRE_SAVE, slot, 0, 0);
          e_seq(E, P, nd->child);
          if (slot >= 0) lp = e_inst(E, DRE_LOOP, slot, 0, E->n + 1);
          {
            int sp = e_inst(E, DRE_SPLIT, 0, 0, 0);
            e_patch(E, sp, body, top);
            e_patch(E, sp, exit, E->n);
            if (lp >= 0) e_patch(E, lp, DRE_X, E->n);
          }
        }
      }
      else {                                  /* '{n,m}': n copies, then m-n
                                                 optional ones */
        int echain = -1;
        for (i = 0; i < nd->min; i++) e_seq(E, P, nd->child);
        for (i = nd->min; i < nd->max; i++) {
          int sp = e_inst(E, DRE_SPLIT, 0, 0, 0);
          e_patch(E, sp, body, E->n);
          e_link(E, &echain, sp, exit);
          e_seq(E, P, nd->child);
          if (E->over) break;                 /* stop multiplying a program
                                                 that is already refused */
        }
        e_patchchain(E, echain, exit, E->n);
      }
      break;
    }
    default:
      break;
  }
}

static void e_seq (dre_emit *E, dre_parser *P, int head) {
  int n;
  for (n = head; n >= 0; n = P->nodes[n].next)
    e_node(E, P, n);
}


/*
** Parse and compile one pattern. Returns 0, or -1 with the reason in 'P->err'
** -- never raises, because the caller owns a buffer that has to be released
** whatever happens, and a longjmp past the release is how leaks are written.
*/
static int dre_build (dre_parser *P, const char *pat, size_t len,
                      unsigned flags, dre_emit *E) {
  int head;
  P->p = P->start = pat;
  P->end = pat + len;
  P->flags = flags;
  P->ngroups = 0;
  P->seenatom = 0;
  P->nnodes = 0;
  P->nclass = 0;
  P->err[0] = '\0';
  memset(P->names, 0, (size_t)(DRE_MAX_GROUPS + 1) * DRE_MAX_NAME);
  head = dre_alt(P, 0);
  if (head < 0) return -1;
  if (P->p != P->end) {
    /* 'dre_concat' stops at ')' and only a group consumes one */
    return dre_perr(P, "unmatched ')'");
  }
  E->base = 2 * (P->ngroups + 1);             /* hidden slots come after the
                                                 capture slots, which are only
                                                 counted once the parse is done */
  E->nhidden = 0;
  e_inst(E, DRE_SAVE, 0, 0, 0);               /* slot 0: where the match began */
  e_seq(E, P, head);
  e_inst(E, DRE_SAVE, 1, 0, 0);               /* slot 1: where it ended */
  e_inst(E, DRE_MATCH, 0, 0, 0);
  if (E->over) {
    char msg[96];
    snprintf(msg, sizeof(msg),
             "pattern too complex: over %d instructions", DRE_MAX_INST);
    P->p = P->end;
    return dre_perr(P, msg);
  }
  return 0;
}


/* ======================================================================
** depth: the program, on the wire
** ====================================================================== */

typedef struct dre_prog {
  const unsigned char *code;   /* ninst * DRE_ISZ */
  const unsigned char *cls;    /* nclass * DRE_CLSZ */
  int ninst, nclass;
  int ncaps;                   /* capture pairs, the whole match included */
  int nhidden;                 /* progress slots for empty-capable loops */
  int nslots;                  /* 2 * ncaps + nhidden */
  unsigned flags;
} dre_prog;

/* Build the byte string a compiled regex carries. */
static void dre_assemble (lua_State *L, dre_parser *P, dre_emit *E) {
  luaL_Buffer b;
  char hdr[DRE_HDR];
  int ncaps = P->ngroups + 1;
  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 'D'; hdr[1] = 'R'; hdr[2] = 'X'; hdr[3] = DRE_VERSION;
  hdr[4] = (char)P->flags;
  hdr[5] = (char)ncaps;
  hdr[6] = (char)((E->n >> 8) & 0xFF); hdr[7] = (char)(E->n & 0xFF);
  hdr[8] = (char)((P->nclass >> 8) & 0xFF); hdr[9] = (char)(P->nclass & 0xFF);
  hdr[10] = (char)E->nhidden;
  luaL_buffinit(L, &b);
  luaL_addlstring(&b, hdr, DRE_HDR);
  luaL_addlstring(&b, (const char *)E->code, (size_t)E->n * DRE_ISZ);
  luaL_addlstring(&b, (const char *)P->classes,
                      (size_t)P->nclass * DRE_CLSZ);
  luaL_pushresult(&b);
}

/*
** Read a program back, refusing anything that is not one.
**
** This is where a forged object is stopped: the program is an ordinary string
** field of an ordinary table, so a program *can* hand this a string it made up.
** Everything the simulator later trusts -- the two counts, and therefore every
** bound it checks an operand against -- is established here, and the operands
** themselves are re-checked as they are used, so the worst a made-up program
** can do is fail to match.
*/
static int dre_open (const char *s, size_t len, dre_prog *pr) {
  const unsigned char *p = (const unsigned char *)s;
  if (len < DRE_HDR || p[0] != 'D' || p[1] != 'R' || p[2] != 'X' ||
      p[3] != DRE_VERSION)
    return 0;
  pr->flags = p[4];
  pr->ncaps = p[5];
  pr->ninst = (p[6] << 8) | p[7];
  pr->nclass = (p[8] << 8) | p[9];
  pr->nhidden = p[10];
  if (pr->ncaps < 1 || pr->ncaps > DRE_MAX_GROUPS + 1 ||
      pr->ninst < 1 || pr->ninst > DRE_MAX_INST ||
      pr->nclass < 0 || pr->nclass > DRE_MAX_CLASS ||
      pr->nhidden > DRE_MAX_LOOPS)
    return 0;
  if (len != (size_t)DRE_HDR + (size_t)pr->ninst * DRE_ISZ +
             (size_t)pr->nclass * DRE_CLSZ)
    return 0;
  pr->nslots = pr->ncaps * 2 + pr->nhidden;
  pr->code = p + DRE_HDR;
  pr->cls = pr->code + (size_t)pr->ninst * DRE_ISZ;
  return 1;
}


/* ======================================================================
** depth: the simulator -- Pike's VM over a Thompson NFA
** ====================================================================== */

/*
** Every thread alive at one position is one entry in a list, and a program
** counter appears in a list at most once ('seen'), which is what bounds the
** whole run: at most 'ninst' threads per position, one pass per byte, so
** O(len * ninst) with no backtracking and nothing that depends on the shape
** of the input rather than its size.
**
** Threads are in priority order, and the epsilon closure is walked
** depth-first with the preferred branch first, so 'a|ab' prefers 'a' and 'a*'
** prefers the longer run -- Perl's answer, reached without Perl's cost. When a
** thread reaches DRE_MATCH the run keeps only the threads that were ahead of
** it, which is the whole implementation of leftmost-first.
*/

typedef struct dre_thr { int kind, a, b; } dre_thr;   /* the closure's stack */

typedef struct dre_list {
  int n;
  int *pc;
  int *caps;                   /* n * nslots */
  unsigned *seen;
  unsigned gen;
} dre_list;

typedef struct dre_vm {
  const unsigned char *code, *cls;
  int ninst, nclass, nslots;
  const char *s;
  size_t len;
  dre_list la, lb;
  int *scratch;                /* nslots, the seed thread's captures */
  dre_thr *stk;
  int stkmax;
} dre_vm;

/* Bytes one run needs, for the caller to allocate in one piece. */
static size_t dre_vmsize (const dre_prog *pr) {
  size_t ni = (size_t)pr->ninst, ns = (size_t)pr->nslots;
  return 2 * (ni * sizeof(int) + ni * ns * sizeof(int) + ni * sizeof(unsigned))
         + ns * sizeof(int)
         + (2 * ni + 8) * sizeof(dre_thr);
}

static void dre_vminit (dre_vm *V, const dre_prog *pr, void *mem,
                        const char *s, size_t len) {
  char *m = (char *)mem;
  size_t ni = (size_t)pr->ninst, ns = (size_t)pr->nslots;
  int i;
  V->code = pr->code; V->cls = pr->cls;
  V->ninst = pr->ninst; V->nclass = pr->nclass; V->nslots = pr->nslots;
  V->s = s; V->len = len;
  for (i = 0; i < 2; i++) {
    dre_list *l = (i == 0) ? &V->la : &V->lb;
    l->pc = (int *)m;             m += ni * sizeof(int);
    l->caps = (int *)m;           m += ni * ns * sizeof(int);
    l->seen = (unsigned *)m;      m += ni * sizeof(unsigned);
    memset(l->seen, 0, ni * sizeof(unsigned));
    l->n = 0;
    l->gen = 1;
  }
  V->scratch = (int *)m;          m += ns * sizeof(int);
  V->stk = (dre_thr *)m;
  V->stkmax = (int)(2 * ni + 8);
}

static void dre_clear (dre_vm *V, dre_list *l) {
  l->n = 0;
  if (++l->gen == 0) {                        /* after 4 billion positions */
    memset(l->seen, 0, (size_t)V->ninst * sizeof(unsigned));
    l->gen = 1;
  }
}

static int dre_isword (const dre_vm *V, size_t i) {
  return dre_isclass(DRE_C_WORD, (unsigned char)V->s[i]);
}

static int dre_holds (const dre_vm *V, int kind, size_t sp) {
  switch (kind) {
    case DRE_A_BOT: return sp == 0;
    case DRE_A_EOT: return sp == V->len;
    case DRE_A_BOL: return sp == 0 || V->s[sp - 1] == '\n';
    case DRE_A_EOL: return sp == V->len || V->s[sp] == '\n';
    case DRE_A_WORDB: case DRE_A_NWORDB: {
      int before = (sp > 0) && dre_isword(V, sp - 1);
      int after = (sp < V->len) && dre_isword(V, sp);
      return (before != after) == (kind == DRE_A_WORDB);
    }
    default: return 0;
  }
}

static int dre_inclass (const dre_vm *V, int idx, int c) {
  if (idx < 0 || idx >= V->nclass) return 0;  /* a forged program */
  return dre_bget(V->cls + (size_t)idx * DRE_CLSZ, c);
}

/*
** Follow every epsilon edge out of 'pc' and add the threads that come to rest
** on a byte test (or on MATCH) to 'l'.
**
** Iterative, not recursive, because the depth is the program's and a program
** may be 512 instructions of nested alternation -- and because this runs once
** per byte per thread, where a C stack frame per edge would be the cost.
** 'caps' is the caller's array and is written through: a SAVE pushes the old
** value with a restore marker, so the array is exactly as it was when this
** returns, and every thread that came to rest took its own copy on the way.
*/
#define DRE_STK_PC       0
#define DRE_STK_RESTORE  1

static void dre_add (dre_vm *V, dre_list *l, int pc, size_t sp, int *caps) {
  int top = 0;
  V->stk[top].kind = DRE_STK_PC; V->stk[top].a = pc; top++;
  while (top > 0) {
    const unsigned char *q;
    int x, y;
    dre_thr e = V->stk[--top];
    if (e.kind == DRE_STK_RESTORE) {
      caps[e.a] = e.b;
      continue;
    }
    pc = e.a;
    if (pc < 0 || pc >= V->ninst) continue;   /* a forged program */
    if (l->seen[pc] == l->gen) continue;      /* already alive here */
    l->seen[pc] = l->gen;
    if (top + 2 > V->stkmax) continue;        /* cannot happen: each pc is
                                                 visited once and pushes two */
    q = V->code + (size_t)pc * DRE_ISZ;
    x = (q[2] << 8) | q[3];
    y = (q[4] << 8) | q[5];
    switch (q[0]) {
      case DRE_JMP:
        V->stk[top].kind = DRE_STK_PC; V->stk[top].a = x; top++;
        break;
      case DRE_SPLIT:                         /* y first, so x is taken first */
        V->stk[top].kind = DRE_STK_PC; V->stk[top].a = y; top++;
        V->stk[top].kind = DRE_STK_PC; V->stk[top].a = x; top++;
        break;
      case DRE_SAVE: {
        int slot = q[1];
        if (slot >= 0 && slot < V->nslots) {
          V->stk[top].kind = DRE_STK_RESTORE;
          V->stk[top].a = slot;
          V->stk[top].b = caps[slot];
          top++;
          caps[slot] = (int)sp;
        }
        V->stk[top].kind = DRE_STK_PC; V->stk[top].a = pc + 1; top++;
        break;
      }
      case DRE_ASSERT:
        if (dre_holds(V, q[1], sp)) {
          V->stk[top].kind = DRE_STK_PC; V->stk[top].a = pc + 1; top++;
        }
        break;
      case DRE_LOOP: {                        /* an empty iteration ends it */
        int slot = q[1];
        int t = (slot < V->nslots && caps[slot] == (int)sp) ? x : y;
        V->stk[top].kind = DRE_STK_PC; V->stk[top].a = t; top++;
        break;
      }
      default:                                /* CHAR, ANY, CLASS, MATCH */
        l->pc[l->n] = pc;
        memcpy(l->caps + (size_t)l->n * V->nslots, caps,
               (size_t)V->nslots * sizeof(int));
        l->n++;
        break;
    }
  }
}

/*
** Run the program over s[startpos..], leftmost-first. 'anchored' seeds only at
** 'startpos', which is what a scan that has already fixed the start needs (and
** what '\A' would do anyway, at the cost of the seed threads it takes to find
** out). Returns 1 with 'out' holding 2 * ncaps offsets, -1 for unset.
*/
static int dre_search (dre_vm *V, size_t startpos, int anchored, int *out) {
  dre_list *cl = &V->la, *nl = &V->lb;
  int matched = 0;
  size_t sp;
  dre_clear(V, cl);
  dre_clear(V, nl);
  for (sp = startpos; ; sp++) {
    int i, c;
    if (!matched && (!anchored || sp == startpos)) {
      for (i = 0; i < V->nslots; i++) V->scratch[i] = -1;
      dre_add(V, cl, 0, sp, V->scratch);      /* a later start is lower
                                                 priority: it is seeded last */
    }
    if (cl->n == 0 && (matched || anchored)) break;
    c = (sp < V->len) ? (unsigned char)V->s[sp] : -1;
    for (i = 0; i < cl->n; i++) {
      int pc = cl->pc[i];
      int *caps = cl->caps + (size_t)i * V->nslots;
      const unsigned char *q = V->code + (size_t)pc * DRE_ISZ;
      switch (q[0]) {
        case DRE_CHAR:
          if (c == (int)q[1]) dre_add(V, nl, pc + 1, sp + 1, caps);
          break;
        case DRE_ANY:
          if (c >= 0 && (q[1] || c != '\n')) dre_add(V, nl, pc + 1, sp + 1, caps);
          break;
        case DRE_CLASS:
          if (c >= 0 && dre_inclass(V, q[1], c))
            dre_add(V, nl, pc + 1, sp + 1, caps);
          break;
        case DRE_MATCH:
          memcpy(out, caps, (size_t)V->nslots * sizeof(int));
          matched = 1;
          i = cl->n;                          /* cut the lower-priority threads */
          break;
        default:
          break;                              /* a forged program: no edge */
      }
    }
    {                                         /* the next position's list */
      dre_list *t = cl; cl = nl; nl = t;
      dre_clear(V, nl);
    }
    if (sp >= V->len) break;
  }
  return matched;
}


/* ======================================================================
** depth: the Lua-facing object
** ====================================================================== */

static const char DRE_MT = 0;       /* registry: the shared metatable      */
static const char DRE_CACHE = 0;    /* registry: pattern -> compiled regex */
static const char DRE_CACHEN = 0;   /* registry: how many are in it        */

static int dre_l_tostring (lua_State *L) {
  lua_getfield(L, 1, "source");
  lua_pushfstring(L, "regex(%s)", luaL_optstring(L, -1, "?"));
  return 1;
}

LUA_API void diluvium_regex_pushmt (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DRE_MT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 3);
    luaL_newlib(L, dre_methods);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "regex");
    lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, dre_l_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DRE_MT);
  }
}

static int dre_isregex (lua_State *L, int idx) {
  int eq;
  if (!lua_istable(L, idx) || !lua_getmetatable(L, idx))
    return 0;
  diluvium_regex_pushmt(L);
  eq = lua_rawequal(L, -1, -2);
  lua_pop(L, 2);
  return eq;
}


/*
** The compile cache, and why it is load-bearing rather than an optimisation.
**
** A `\d+` literal desugars to a call to 'regex.compile' (lparser.c), so a
** literal inside a loop reaches this function on every iteration. Without the
** cache that is a parse and a compile per iteration; with it, it is a hash
** lookup on a string the caller already holds.
**
** Bounded and flushed wholesale rather than evicted one at a time: a program
** that generates patterns should not be able to grow this without limit, and
** an LRU would need a second structure to hold an order that no measurement
** here has ever asked for. A compile with explicit flags does not consult the
** cache at all -- the key would have to carry the flags, and the paths that
** repeat are the literal and the plain 'regex.compile(pattern)'.
*/
static void dre_cachetable (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DRE_CACHE) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DRE_CACHE);
    lua_pushinteger(L, 0);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DRE_CACHEN);
  }
}

/* Pushes the cached regex for the pattern at 'patidx', or nothing; returns 1
   when something was pushed. */
static int dre_cacheget (lua_State *L, int patidx) {
  dre_cachetable(L);
  lua_pushvalue(L, patidx);
  if (lua_rawget(L, -2) == LUA_TTABLE) {
    lua_remove(L, -2);                        /* the cache table */
    return 1;
  }
  lua_pop(L, 2);
  return 0;
}

static void dre_cacheput (lua_State *L, int patidx, int objidx) {
  lua_Integer n;
  dre_cachetable(L);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &DRE_CACHEN);
  n = lua_tointeger(L, -1);
  lua_pop(L, 1);
  if (n >= DRE_CACHE_MAX) {                   /* flush, do not evict */
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DRE_CACHE);
    n = 0;
  }
  lua_pushvalue(L, patidx);
  lua_pushvalue(L, objidx);
  lua_rawset(L, -3);
  lua_pop(L, 1);                              /* the cache table */
  lua_pushinteger(L, n + 1);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DRE_CACHEN);
}


/* The scratch one compile needs: nodes, classes, code and names, in one
   allocation held as a userdata so that an error unwinds without leaking. */
#define DRE_SCRATCH ((size_t)DRE_MAX_NODES * sizeof(dre_node) + \
                     (size_t)DRE_MAX_CLASS * DRE_CLSZ + \
                     (size_t)DRE_MAX_INST * DRE_ISZ + \
                     (size_t)(DRE_MAX_GROUPS + 1) * DRE_MAX_NAME)

static void dre_flagstring (unsigned flags, char *out) {
  int n = 0;
  if (flags & DRE_F_ICASE) out[n++] = 'i';
  if (flags & DRE_F_MULTI) out[n++] = 'm';
  if (flags & DRE_F_DOTALL) out[n++] = 's';
  out[n] = '\0';
}

/* Compile the pattern at 'patidx' and push the regex object. */
static void dre_docompile (lua_State *L, int patidx, unsigned flags) {
  dre_parser P;
  dre_emit E;
  size_t len;
  const char *pat = lua_tolstring(L, patidx, &len);
  char *m;
  int g, named = 0;
  char fs[4];
  if (len > DRE_MAX_PATTERN)
    luaL_error(L, "regex: pattern longer than %d bytes", (int)DRE_MAX_PATTERN);
  m = (char *)lua_newuserdatauv(L, DRE_SCRATCH, 0);
  P.nodes = (dre_node *)m;      m += (size_t)DRE_MAX_NODES * sizeof(dre_node);
  P.classes = (unsigned char *)m; m += (size_t)DRE_MAX_CLASS * DRE_CLSZ;
  E.code = (unsigned char *)m;  m += (size_t)DRE_MAX_INST * DRE_ISZ;
  P.names = (char (*)[DRE_MAX_NAME])m;
  E.n = 0; E.max = DRE_MAX_INST; E.over = 0;
  if (dre_build(&P, pat, len, flags, &E) < 0) {
    char msg[sizeof(P.err)];
    memcpy(msg, P.err, sizeof(msg));
    lua_pop(L, 1);                            /* the scratch */
    luaL_error(L, "regex: %s", msg);
  }
  lua_createtable(L, 1, 4);
  dre_assemble(L, &P, &E);
  lua_rawseti(L, -2, 1);                      /* [1] = the program */
  lua_pushvalue(L, patidx);
  lua_setfield(L, -2, "source");
  dre_flagstring(P.flags, fs);
  lua_pushstring(L, fs);
  lua_setfield(L, -2, "flags");
  lua_pushinteger(L, P.ngroups);
  lua_setfield(L, -2, "ngroups");
  for (g = 1; g <= P.ngroups; g++)
    if (P.names[g][0] != '\0') named++;
  if (named > 0) {
    lua_createtable(L, 0, named);
    for (g = 1; g <= P.ngroups; g++) {
      if (P.names[g][0] != '\0') {
        lua_pushinteger(L, g);
        lua_setfield(L, -2, P.names[g]);
      }
    }
    lua_setfield(L, -2, "names");
  }
  diluvium_regex_pushmt(L);
  lua_setmetatable(L, -2);
  lua_remove(L, -2);                          /* the scratch, now unread */
}

static int dre_l_compile (lua_State *L) {
  unsigned flags = 0;
  if (dre_isregex(L, 1) && lua_isnoneornil(L, 2)) {
    lua_settop(L, 1);                         /* already compiled */
    return 1;
  }
  luaL_checktype(L, 1, LUA_TSTRING);
  if (!lua_isnoneornil(L, 2)) {
    const char *f = luaL_checkstring(L, 2);
    for (; *f != '\0'; f++) {
      switch (*f) {
        case 'i': flags |= DRE_F_ICASE; break;
        case 'm': flags |= DRE_F_MULTI; break;
        case 's': flags |= DRE_F_DOTALL; break;
        default:
          return luaL_error(L, "regex: unknown flag '%c' (the flags are "
                               "'i', 'm' and 's')", *f);
      }
    }
  }
  if (flags == 0 && dre_cacheget(L, 1))
    return 1;
  dre_docompile(L, 1, flags);
  if (flags == 0)
    dre_cacheput(L, 1, lua_gettop(L));
  return 1;
}

/* The first argument of every other function: a compiled regex, or a pattern
   to compile (through the cache, so a string in a loop compiles once). */
static int dre_argregex (lua_State *L, int idx) {
  if (dre_isregex(L, idx)) {
    lua_pushvalue(L, idx);
    return lua_gettop(L);
  }
  if (lua_type(L, idx) == LUA_TSTRING) {
    if (!dre_cacheget(L, idx)) {
      dre_docompile(L, idx, 0);
      dre_cacheput(L, idx, lua_gettop(L));
    }
    return lua_gettop(L);
  }
  luaL_argerror(L, idx, lua_pushfstring(L, "regex or string expected, got %s",
                                        luaL_typename(L, idx)));
  return 0;                                   /* unreachable */
}


/* ======================================================================
** depth: matching, and the five functions that use it
** ====================================================================== */

typedef struct dre_ctx {
  dre_prog pr;
  dre_vm vm;
  int *caps;                   /* nslots offsets, -1 for a group that did
                                  not take part */
} dre_ctx;

/*
** Open a regex for matching. Leaves two values on the stack -- the program
** string, which anchors the bytes the simulator reads, and the run's memory
** as a userdata -- and the caller leaves them there: they are below whatever
** it returns, and the GC is what releases them if a replacement function
** raises halfway through a gsub.
*/
static void dre_ctxopen (lua_State *L, dre_ctx *C, int objidx,
                         const char *s, size_t len) {
  const char *prog;
  size_t plen;
  void *mem;
  if (lua_rawgeti(L, objidx, 1) != LUA_TSTRING)
    luaL_error(L, "regex: not a compiled regex (its program is missing)");
  prog = lua_tolstring(L, -1, &plen);
  if (!dre_open(prog, plen, &C->pr))
    luaL_error(L, "regex: not a compiled regex (its program is malformed)");
  mem = lua_newuserdatauv(L,
          dre_vmsize(&C->pr) + (size_t)C->pr.nslots * sizeof(int), 0);
  dre_vminit(&C->vm, &C->pr, mem, s, len);
  C->caps = (int *)((char *)mem + dre_vmsize(&C->pr));
}

static lua_Integer dre_initpos (lua_State *L, int arg, size_t len) {
  lua_Integer init = luaL_optinteger(L, arg, 1);
  if (init > 0)
    init--;                                   /* 1-based, like string.find */
  else if (init < 0) {
    init += (lua_Integer)len;                 /* from the end */
    if (init < 0) init = 0;
  }
  return init;
}

/*
** Push the capture group values of the last match. A group that did not take
** part pushes 'false' rather than nil: a table built from the result then
** keeps its shape, and 'if caps[2] then' still reads the way it looks.
*/
static int dre_pushcaptures (lua_State *L, dre_ctx *C, const char *s,
                             size_t len) {
  int i;
  luaL_checkstack(L, C->pr.ncaps + 2, "too many captures");
  for (i = 1; i < C->pr.ncaps; i++) {
    int a = C->caps[i * 2], b = C->caps[i * 2 + 1];
    if (a < 0 || b < a || (size_t)b > len)
      lua_pushboolean(L, 0);
    else
      lua_pushlstring(L, s + a, (size_t)(b - a));
  }
  return C->pr.ncaps - 1;
}

/* The whole match, or the captures when the pattern has any: what
   'string.match' does, and what a caller of gmatch or a gsub replacement
   function is handed. */
static int dre_pushresult (lua_State *L, dre_ctx *C, const char *s,
                           size_t len) {
  if (C->pr.ncaps == 1) {
    int a = C->caps[0], b = C->caps[1];
    lua_pushlstring(L, s + a, (size_t)(b - a));
    return 1;
  }
  return dre_pushcaptures(L, C, s, len);
}


static int dre_l_find (lua_State *L) {
  dre_ctx C;
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  lua_Integer init = dre_initpos(L, 3, len);
  int obj;                                    /* read the plain arguments
                                                 first: 'dre_argregex' pushes,
                                                 and a pushed value would be
                                                 argument 3 to everything after
                                                 it */
  if (init > (lua_Integer)len) {
    lua_pushnil(L);
    return 1;
  }
  obj = dre_argregex(L, 1);
  dre_ctxopen(L, &C, obj, s, len);
  if (!dre_search(&C.vm, (size_t)init, 0, C.caps)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, C.caps[0] + 1);          /* 1-based, inclusive */
  lua_pushinteger(L, C.caps[1]);
  return 2 + dre_pushcaptures(L, &C, s, len);
}

static int dre_l_match (lua_State *L) {
  dre_ctx C;
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  lua_Integer init = dre_initpos(L, 3, len);
  int obj;                                    /* read the plain arguments
                                                 first: 'dre_argregex' pushes,
                                                 and a pushed value would be
                                                 argument 3 to everything after
                                                 it */
  if (init > (lua_Integer)len) {
    lua_pushnil(L);
    return 1;
  }
  obj = dre_argregex(L, 1);
  dre_ctxopen(L, &C, obj, s, len);
  if (!dre_search(&C.vm, (size_t)init, 0, C.caps)) {
    lua_pushnil(L);
    return 1;
  }
  return dre_pushresult(L, &C, s, len);
}


/*
** gmatch. The iterator keeps the regex, the subject, the position and where
** the last match ended as upvalues, and the run's memory as another --
** allocated once here rather than on every step, which is the difference
** between one allocation per loop and one per match.
**
** The scan rule is 'string.gmatch''s, to the letter: a match that ends where
** the previous one ended is not a match. That is what stops '\\d*' from
** reporting an empty match immediately after every number it finds, and it is
** taken from lstrlib.c rather than invented, because the two functions are
** each other's siblings and a Lua programmer's habits have to carry over.
*/
static int dre_gmatch_iter (lua_State *L) {
  dre_ctx C;
  size_t len, plen;
  const char *s = lua_tolstring(L, lua_upvalueindex(2), &len);
  const char *prog;
  lua_Integer pos = lua_tointeger(L, lua_upvalueindex(3));
  void *mem;
  if (pos > (lua_Integer)len) return 0;
  if (lua_rawgeti(L, lua_upvalueindex(1), 1) != LUA_TSTRING)
    return luaL_error(L, "regex: not a compiled regex (its program is missing)");
  prog = lua_tolstring(L, -1, &plen);
  if (!dre_open(prog, plen, &C.pr))
    return luaL_error(L, "regex: not a compiled regex (its program is "
                         "malformed)");
  mem = lua_touserdata(L, lua_upvalueindex(4));
  if (mem == NULL ||
      lua_rawlen(L, lua_upvalueindex(4)) <
        dre_vmsize(&C.pr) + (size_t)C.pr.nslots * sizeof(int))
    return luaL_error(L, "regex: the regex changed under an active gmatch");
  dre_vminit(&C.vm, &C.pr, mem, s, len);
  C.caps = (int *)((char *)mem + dre_vmsize(&C.pr));
  for (;;) {                                  /* 'string.gmatch''s own rule */
    if (pos > (lua_Integer)len || !dre_search(&C.vm, (size_t)pos, 0, C.caps))
      return 0;
    if (C.caps[0] == C.caps[1] &&
        C.caps[1] == lua_tointeger(L, lua_upvalueindex(5))) {
      pos = C.caps[0] + 1;                    /* an empty match where the last
                                                 one ended is not a match */
      continue;
    }
    break;
  }
  lua_pushinteger(L, C.caps[1]);
  lua_replace(L, lua_upvalueindex(3));
  lua_pushinteger(L, C.caps[1]);
  lua_replace(L, lua_upvalueindex(5));
  return dre_pushresult(L, &C, s, len);
}

static int dre_l_gmatch (lua_State *L) {
  dre_ctx C;
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  lua_Integer init = dre_initpos(L, 3, len);
  int obj = dre_argregex(L, 1);               /* after the plain arguments */
  lua_pushvalue(L, obj);                      /* upvalue 1: the regex */
  lua_pushvalue(L, 2);                        /* upvalue 2: the subject */
  lua_pushinteger(L, init);                   /* upvalue 3: the position */
  dre_ctxopen(L, &C, obj, s, len);            /* program string, then memory */
  lua_remove(L, -2);                          /* the program string: the
                                                 regex upvalue anchors it */
  lua_pushinteger(L, -1);                     /* upvalue 5: where the last
                                                 match ended, or -1 */
  lua_pushcclosure(L, dre_gmatch_iter, 5);
  return 1;
}


/* ---- gsub ------------------------------------------------------------ */

/* One match's replacement, appended to 'b'. Mirrors 'string.gsub': a string
   with '%1'..'%9' and '%0', a table indexed by the first capture, or a
   function called with the captures. */
static void dre_addvalue (lua_State *L, dre_ctx *C, luaL_Buffer *b,
                          int replidx, const char *s, size_t len) {
  int a = C->caps[0], e = C->caps[1];
  switch (lua_type(L, replidx)) {
    case LUA_TSTRING: case LUA_TNUMBER: {
      size_t rl;
      const char *r = lua_tolstring(L, replidx, &rl);
      size_t i;
      for (i = 0; i < rl; i++) {
        if (r[i] != '%')
          luaL_addchar(b, r[i]);
        else if (++i >= rl)
          luaL_error(L, "regex.gsub: a '%%' at the end of the replacement "
                        "string");
        else if (r[i] == '%')
          luaL_addchar(b, '%');
        else if (r[i] >= '0' && r[i] <= '9') {
          int g = r[i] - '0';
          if (g >= C->pr.ncaps)
            luaL_error(L, "regex.gsub: '%%%d' but the pattern has %d capture "
                          "groups", g, C->pr.ncaps - 1);
          if (g == 0)
            luaL_addlstring(b, s + a, (size_t)(e - a));
          else if (C->caps[g * 2] >= 0)
            luaL_addlstring(b, s + C->caps[g * 2],
                            (size_t)(C->caps[g * 2 + 1] - C->caps[g * 2]));
          /* a group that did not take part contributes nothing */
        }
        else
          luaL_error(L, "regex.gsub: '%%%c' is not a replacement item "
                        "(use '%%1'..'%%9', '%%0' or '%%%%')", r[i]);
      }
      return;
    }
    case LUA_TTABLE: {
      if (C->pr.ncaps > 1) {
        int g = C->caps[2], h = C->caps[3];
        if (g < 0) lua_pushboolean(L, 0);
        else lua_pushlstring(L, s + g, (size_t)(h - g));
      }
      else
        lua_pushlstring(L, s + a, (size_t)(e - a));
      lua_gettable(L, replidx);
      break;
    }
    case LUA_TFUNCTION: {
      int n;
      lua_pushvalue(L, replidx);
      n = dre_pushresult(L, C, s, len);
      lua_call(L, n, 1);
      break;
    }
    default:
      luaL_error(L, "regex.gsub: the replacement must be a string, a table or "
                    "a function");
      return;
  }
  /* the table or the function answered */
  if (!lua_toboolean(L, -1)) {                /* nil or false: keep the text */
    lua_pop(L, 1);
    luaL_addlstring(b, s + a, (size_t)(e - a));
  }
  else if (!lua_isstring(L, -1))
    luaL_error(L, "regex.gsub: the replacement produced a %s",
               luaL_typename(L, -1));
  else
    luaL_addvalue(b);                          /* pops it */
}

static int dre_l_gsub (lua_State *L) {
  dre_ctx C;
  luaL_Buffer b;
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  lua_Integer maxn = luaL_optinteger(L, 4, -1);
  lua_Integer count = 0;
  lua_Integer lastend = -1;
  size_t pos = 0;
  int t = lua_type(L, 3);
  int obj;
  luaL_argexpected(L, t == LUA_TSTRING || t == LUA_TNUMBER ||
                      t == LUA_TTABLE || t == LUA_TFUNCTION, 3,
                   "string, table or function");
  obj = dre_argregex(L, 1);                   /* after the plain arguments */
  dre_ctxopen(L, &C, obj, s, len);
  luaL_buffinit(L, &b);
  while (maxn < 0 || count < maxn) {
    size_t a, e;
    if (pos > len || !dre_search(&C.vm, pos, 0, C.caps)) break;
    a = (size_t)C.caps[0];
    e = (size_t)C.caps[1];
    if (a == e && lastend >= 0 && a == (size_t)lastend) {
      /* 'string.gsub''s rule, and the reason this is not simply "an empty
         match advances one byte": a match that ends where the last one ended
         is refused, so 'a*' over "aaa" replaces once rather than twice. */
      if (a >= len) break;
      luaL_addlstring(&b, s + pos, a - pos + 1);   /* that byte comes along */
      pos = a + 1;
      continue;
    }
    luaL_addlstring(&b, s + pos, a - pos);    /* the text before the match */
    dre_addvalue(L, &C, &b, 3, s, len);
    count++;
    lastend = (lua_Integer)e;
    pos = e;                                  /* the rule above is what makes
                                                 an empty match still progress */
  }
  if (pos < len)
    luaL_addlstring(&b, s + pos, len - pos);
  luaL_pushresult(&b);
  lua_pushinteger(L, count);
  return 2;
}


/* ---- split and escape ------------------------------------------------ */

/*
** split. Only a non-empty match separates: a pattern that can match nothing
** would otherwise split between every byte, or worse, not terminate -- and
** "the separator matched nothing, so nothing was separated" is the rule that
** needs no footnote. 'limit' caps the number of pieces; the last one holds
** whatever is left, unsplit.
*/
static int dre_l_split (lua_State *L) {
  dre_ctx C;
  size_t len;
  const char *s = luaL_checklstring(L, 2, &len);
  lua_Integer limit = luaL_optinteger(L, 3, -1);
  lua_Integer n = 0;
  size_t pos = 0, scan = 0;
  int obj;
  if (limit <= 0) limit = -1;
  obj = dre_argregex(L, 1);                   /* after the plain arguments */
  dre_ctxopen(L, &C, obj, s, len);
  lua_newtable(L);
  while (limit < 0 || n + 1 < limit) {
    size_t a, e;
    if (scan > len || !dre_search(&C.vm, scan, 0, C.caps)) break;
    a = (size_t)C.caps[0];
    e = (size_t)C.caps[1];
    if (e == a) {                             /* nothing separated here */
      scan = a + 1;
      continue;
    }
    lua_pushlstring(L, s + pos, a - pos);
    lua_rawseti(L, -2, ++n);
    pos = scan = e;
  }
  lua_pushlstring(L, s + pos, len - pos);
  lua_rawseti(L, -2, ++n);
  return 1;
}

static int dre_l_escape (lua_State *L) {
  size_t len, i;
  const char *s = luaL_checklstring(L, 1, &len);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0) {
      luaL_addlstring(&b, "\\x00", 4);        /* '\0' would end nothing here,
                                                 but '\x00' reads better */
    }
    else if (dre_isclass(DRE_C_WORD, c) || c >= 0x80)
      luaL_addchar(&b, (char)c);              /* letters, digits, '_', UTF-8 */
    else {
      luaL_addchar(&b, '\\');                 /* '\<punctuation>' is literal */
      luaL_addchar(&b, (char)c);
    }
  }
  luaL_pushresult(&b);
  return 1;
}


/* ---- registration ---------------------------------------------------- */

LUAMOD_API int luaopen_dregex (lua_State *L) {
  luaL_newlib(L, dre_module);
  diluvium_regex_pushmt(L);                   /* built before any object is */
  lua_pop(L, 1);
  return 1;
}
