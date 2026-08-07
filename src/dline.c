/*
** dline.c
** Diluvium line editor. See dline.h.
*/

#define dline_c

#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "dline.h"
#include "drepl.h"


/*
** A terminal is only usable where there is a termios to put into raw
** mode. Everywhere else -- Windows, WASI, a pipe -- the editor is absent
** and input is read plainly, exactly as stock Lua behaves when built
** without readline.
*/
#if !defined(DILUVIUM_NO_LINEEDIT) && !defined(__wasi__) && \
    !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#define DLINE_TERMIOS	1
#endif


#define DLINE_MAXLINE		4096
#define DLINE_MAXHISTORY	1000


/* ====================================================================== */
/* History                                                                */
/* ====================================================================== */

static char **hist = NULL;
static int histlen = 0;


void diluvium_history_free (void) {
  int i;
  for (i = 0; i < histlen; i++) free(hist[i]);
  free(hist);
  hist = NULL;
  histlen = 0;
}


void diluvium_history_add (const char *line) {
  char *copy;
  if (line == NULL || line[0] == '\0') return;  /* nothing to remember */
  if (histlen > 0 && strcmp(hist[histlen - 1], line) == 0)
    return;  /* the same thing twice running is one entry */
  if (hist == NULL) {
    hist = (char **)malloc(sizeof(char *) * DLINE_MAXHISTORY);
    if (hist == NULL) return;
  }
  copy = (char *)malloc(strlen(line) + 1);
  if (copy == NULL) return;
  strcpy(copy, line);
  if (histlen == DLINE_MAXHISTORY) {  /* full: drop the oldest */
    free(hist[0]);
    memmove(hist, hist + 1, sizeof(char *) * (DLINE_MAXHISTORY - 1));
    histlen--;
  }
  hist[histlen++] = copy;
}


int diluvium_history_load (const char *path) {
  char buf[DLINE_MAXLINE];
  FILE *f;
  if (path == NULL || (f = fopen(path, "r")) == NULL) return -1;
  while (fgets(buf, sizeof(buf), f) != NULL) {
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    diluvium_history_add(buf);
  }
  fclose(f);
  return 0;
}


int diluvium_history_save (const char *path) {
  FILE *f;
  int i;
  if (path == NULL || (f = fopen(path, "w")) == NULL) return -1;
  for (i = 0; i < histlen; i++) fprintf(f, "%s\n", hist[i]);
  fclose(f);
  return 0;
}


const char *diluvium_history_path (void) {
  static char path[1024];
  const char *home = getenv("HOME");
#if defined(_WIN32)
  if (home == NULL) home = getenv("USERPROFILE");
#endif
  if (home == NULL || *home == '\0') return NULL;
  if (strlen(home) + sizeof("/.diluvium_history") > sizeof(path)) return NULL;
  strcpy(path, home);
  strcat(path, "/.diluvium_history");
  return path;
}


/* ====================================================================== */
/* Plain input, used when there is no terminal to edit on                 */
/* ====================================================================== */

static char *readplain (const char *prompt) {
  char buf[DLINE_MAXLINE];
  size_t n;
  char *line;
  if (prompt != NULL) {
    fputs(prompt, stdout);
    fflush(stdout);
  }
  if (fgets(buf, sizeof(buf), stdin) == NULL) return NULL;
  n = strlen(buf);
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
  line = (char *)malloc(n + 1);
  if (line == NULL) return NULL;
  memcpy(line, buf, n + 1);
  return line;
}


void diluvium_freeline (char *line) {
  free(line);
}


#if !defined(DLINE_TERMIOS)		/* { */

char *diluvium_readline (lua_State *L, const char *prompt) {
  (void)L;
  return readplain(prompt);
}

#else					/* }{ */

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>


/* The line being edited. */
typedef struct EditState {
  char *buf;          /* the line itself, always NUL terminated */
  size_t cap;         /* bytes available in 'buf' */
  size_t len;         /* bytes used */
  size_t pos;         /* cursor, as a byte offset into 'buf' */
  const char *prompt;
  size_t plen;        /* display width of the prompt */
  size_t cols;        /* terminal width */
  int hindex;         /* where we are in the history while browsing */
  lua_State *L;       /* for completion; may be NULL */
} EditState;


static struct termios saved_termios;
static int israw = 0;


static int enterraw (void) {
  struct termios raw;
  if (!isatty(STDIN_FILENO)) return -1;
  if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) return -1;
  raw = saved_termios;
  /* no break, no CR-to-NL, no parity check, no strip, no flow control */
  raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(unsigned)(OPOST);
  raw.c_cflag |= (unsigned)(CS8);
  /* read byte by byte, no echo, no canonical mode, no signal characters:
     Ctrl-C and Ctrl-D are handled here rather than by the driver */
  raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;
  israw = 1;
  return 0;
}


static void leaveraw (void) {
  if (israw) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    israw = 0;
  }
}


static size_t termcols (void) {
#ifdef TIOCGWINSZ
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
#endif
  return 80;
}


/*
** UTF-8: a continuation byte is never a character on its own, so the
** cursor steps over whole characters rather than bytes.
*/
static int iscontbyte (unsigned char c) { return (c & 0xC0) == 0x80; }

static size_t nextchar (const char *s, size_t len, size_t i) {
  if (i >= len) return len;
  i++;
  while (i < len && iscontbyte((unsigned char)s[i])) i++;
  return i;
}

static size_t prevchar (const char *s, size_t i) {
  if (i == 0) return 0;
  i--;
  while (i > 0 && iscontbyte((unsigned char)s[i])) i--;
  return i;
}

/* characters, not bytes, between two offsets */
static size_t width (const char *s, size_t from, size_t to) {
  size_t n = 0;
  while (from < to) {
    if (!iscontbyte((unsigned char)s[from])) n++;
    from++;
  }
  return n;
}


/* ====================================================================== */
/* Syntax highlighting                                                    */
/* ====================================================================== */

/*
** Colour classes. The line is classified byte by byte into this small
** set, then drawn as runs of one colour, which keeps the drawing code
** from having to interleave with the scanner.
*/
#define HL_NONE		0
#define HL_KEYWORD	1
#define HL_STRING	2
#define HL_NUMBER	3
#define HL_COMMENT	4
#define HL_DILUVIUM	5   /* what Diluvium adds to Lua: $, ??, ?., ~function */

static const char *const hlcolour[] = {
  "\x1b[0m",    /* none    */
  "\x1b[35m",   /* keyword: magenta */
  "\x1b[32m",   /* string:  green   */
  "\x1b[36m",   /* number:  cyan    */
  "\x1b[90m",   /* comment: grey    */
  "\x1b[33m"    /* Diluvium: yellow, so its own syntax stands out */
};


static const char *const hlwords[] = {
  "and", "break", "case", "default", "defer", "do", "else", "elseif",
  "end", "false", "for", "function", "global", "goto", "if", "in",
  "local", "nil", "not", "or", "repeat", "return", "switch", "then",
  "true", "until", "while", NULL
};


static int iskeyword (const char *s, size_t len) {
  const char *const *w;
  for (w = hlwords; *w != NULL; w++)
    if (strlen(*w) == len && strncmp(*w, s, len) == 0) return 1;
  return 0;
}


static int isdigitc (char c) { return c >= '0' && c <= '9'; }
static int isnamec (char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}


/*
** Classify every byte of the line. The whole line is scanned even when
** only part of it is on screen, because whether a byte sits inside a
** string or a comment depends on everything before it.
**
** This is a display scanner, not the real lexer: it has to make sense of
** half-typed input, where llex would simply raise an error.
*/
static void classify (const char *s, size_t len, unsigned char *cls) {
  size_t i = 0;
  memset(cls, HL_NONE, len);
  while (i < len) {
    if (s[i] == '-' && i + 1 < len && s[i + 1] == '-') {
      while (i < len) cls[i++] = HL_COMMENT;  /* to end of line */
    }
    else if (s[i] == '"' || s[i] == '\'') {
      char q = s[i];
      cls[i++] = HL_STRING;
      while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {   /* an escape, both bytes */
          cls[i++] = HL_STRING;
          cls[i++] = HL_STRING;
          continue;
        }
        cls[i] = HL_STRING;
        if (s[i++] == q) break;             /* closing quote */
      }
    }
    else if (s[i] == '$' && i + 1 < len && (s[i+1] == '"' || s[i+1] == '\'')) {
      cls[i++] = HL_DILUVIUM;               /* the '$' is Diluvium's */
    }
    else if (s[i] == '?' && i + 1 < len && (s[i+1] == '?' || s[i+1] == '.' ||
                                            s[i+1] == '[')) {
      cls[i++] = HL_DILUVIUM;               /* '??', '?.', '?[' */
      if (i < len && s[i] != '[') cls[i++] = HL_DILUVIUM;
    }
    else if (s[i] == '~' && i + 1 + 8 <= len &&
             strncmp(s + i + 1, "function", 8) == 0) {
      cls[i++] = HL_DILUVIUM;               /* a secure '~function' -- and
                                               only that, so '~x' stays the
                                               bitwise operator it is */
    }
    else if (isdigitc(s[i]) ||
             (s[i] == '.' && i + 1 < len && isdigitc(s[i + 1]))) {
      while (i < len && (isnamec(s[i]) || s[i] == '.' ||
                         ((s[i] == '-' || s[i] == '+') && i > 0 &&
                          (s[i-1] == 'e' || s[i-1] == 'E'))))
        cls[i++] = HL_NUMBER;
    }
    else if (isnamec(s[i])) {
      size_t w = i;
      while (i < len && isnamec(s[i])) i++;
      if (iskeyword(s + w, i - w))
        memset(cls + w, HL_KEYWORD, i - w);
    }
    else i++;
  }
}


/* Colour is for a terminal that wants it, and nothing else. */
static int wantcolour (void) {
  static int cached = -1;
  if (cached < 0) {
    const char *term = getenv("TERM");
    cached = isatty(STDOUT_FILENO) &&
             getenv("NO_COLOR") == NULL &&
             getenv("DILUVIUM_NO_COLOR") == NULL &&
             !(term != NULL && strcmp(term, "dumb") == 0);
  }
  return cached;
}


/*
** Redraw the line. Long lines scroll horizontally rather than wrapping,
** which keeps the whole thing on one row and the cursor arithmetic
** honest no matter how the terminal handles the last column.
**
** Only the visible slice is ever emitted, so the colour escapes -- which
** occupy bytes but no columns -- cannot overflow the buffer however long
** the line grows, and the cursor column is counted in characters, so they
** do not disturb it either.
*/
static void refresh (EditState *e) {
  char out[4096];
  unsigned char cls[DLINE_MAXLINE];
  size_t start = 0, end = e->len;
  size_t curcol = width(e->buf, 0, e->pos);
  size_t avail = (e->cols > e->plen + 1) ? e->cols - e->plen - 1 : 1;
  size_t n = 0;
  int colour = wantcolour();
  /* scroll so the cursor stays visible */
  while (curcol >= avail) {
    start = nextchar(e->buf, e->len, start);
    curcol--;
  }
  {
    size_t shown = 0, i = start;
    while (i < e->len && shown < avail) {
      i = nextchar(e->buf, e->len, i);
      shown++;
    }
    end = i;
  }
  out[n++] = '\r';                       /* to column 0 */
  memcpy(out + n, e->prompt, e->plen); n += e->plen;
  if (!colour) {
    memcpy(out + n, e->buf + start, end - start); n += end - start;
  }
  else {
    size_t i = start;
    unsigned char cur = 0xff;             /* no colour emitted yet */
    classify(e->buf, e->len, cls);
    while (i < end && n + 32 < sizeof(out)) {
      if (cls[i] != cur) {
        cur = cls[i];
        n += (size_t)snprintf(out + n, 16, "%s", hlcolour[cur]);
      }
      out[n++] = e->buf[i++];
    }
    if (cur != HL_NONE)
      n += (size_t)snprintf(out + n, 16, "%s", hlcolour[HL_NONE]);
  }
  memcpy(out + n, "\x1b[0K", 4); n += 4; /* erase what used to be there */
  n += (size_t)snprintf(out + n, 32, "\r\x1b[%dC",
                        (int)(e->plen + curcol));
  if (write(STDOUT_FILENO, out, n) < 0) { /* nothing useful to do */ }
}


static int insert (EditState *e, const char *s, size_t n) {
  if (e->len + n >= e->cap) return -1;
  memmove(e->buf + e->pos + n, e->buf + e->pos, e->len - e->pos);
  memcpy(e->buf + e->pos, s, n);
  e->pos += n;
  e->len += n;
  e->buf[e->len] = '\0';
  return 0;
}


static void erase (EditState *e, size_t from, size_t to) {
  if (from >= to) return;
  memmove(e->buf + from, e->buf + to, e->len - to);
  e->len -= to - from;
  e->pos = from;
  e->buf[e->len] = '\0';
}


/* start of the word before the cursor */
static size_t wordstart (EditState *e) {
  size_t i = e->pos;
  while (i > 0 && e->buf[i - 1] == ' ') i--;
  while (i > 0 && e->buf[i - 1] != ' ') i--;
  return i;
}


static void setline (EditState *e, const char *s) {
  size_t n = strlen(s);
  if (n >= e->cap) n = e->cap - 1;
  memcpy(e->buf, s, n);
  e->buf[n] = '\0';
  e->len = e->pos = n;
}


/* ====================================================================== */
/* Completion                                                             */
/* ====================================================================== */

/*
** Tab: with one candidate, finish the word. With several, extend as far
** as they agree and list them, which is what a shell does and what makes
** repeated tabs useful rather than noisy.
*/
static void complete (EditState *e) {
  size_t off, n, i;
  const char *first;
  size_t common;
  if (e->L == NULL) return;
  off = diluvium_repl_complete(e->L, e->buf, e->pos);
  n = (size_t)lua_rawlen(e->L, -1);
  if (n == 0) { lua_pop(e->L, 1); return; }
  lua_rawgeti(e->L, -1, 1);
  first = lua_tostring(e->L, -1);
  common = strlen(first);
  for (i = 2; i <= n; i++) {  /* how far do they all agree? */
    const char *s;
    size_t j = 0;
    lua_rawgeti(e->L, -2, (lua_Integer)i);
    s = lua_tostring(e->L, -1);
    while (j < common && s[j] != '\0' && s[j] == first[j]) j++;
    common = j;
    lua_pop(e->L, 1);
  }
  if (common > e->pos - off) {  /* something to add */
    char add[DLINE_MAXLINE];
    size_t addlen = common - (e->pos - off);
    if (addlen < sizeof(add)) {
      memcpy(add, first + (e->pos - off), addlen);
      insert(e, add, addlen);
    }
  }
  else if (n > 1) {  /* already at the common prefix: show the choices */
    if (write(STDOUT_FILENO, "\r\n", 2) < 0) { }
    for (i = 1; i <= n; i++) {
      const char *s;
      lua_rawgeti(e->L, -2, (lua_Integer)i);
      s = lua_tostring(e->L, -1);
      if (write(STDOUT_FILENO, s, strlen(s)) < 0) { }
      if (write(STDOUT_FILENO, (i % 4 == 0) ? "\r\n" : "\t", (i % 4 == 0) ? 2 : 1) < 0) { }
      lua_pop(e->L, 1);
    }
    if (n % 4 != 0) { if (write(STDOUT_FILENO, "\r\n", 2) < 0) { } }
  }
  lua_pop(e->L, 2);  /* the first candidate and the table */
  refresh(e);
}


/* ====================================================================== */
/* The edit loop                                                          */
/* ====================================================================== */

#define DCTRL(c)		((c) & 0x1f)


static int editline (EditState *e) {
  refresh(e);
  for (;;) {
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return (e->len > 0) ? 0 : -1;
    switch (c) {
      case '\r': case '\n': {
        if (write(STDOUT_FILENO, "\r\n", 2) < 0) { }
        return 0;
      }
      case DCTRL('c'): {  /* abandon this line, but keep the session */
        e->len = e->pos = 0;
        e->buf[0] = '\0';
        if (write(STDOUT_FILENO, "^C\r\n", 4) < 0) { }
        return 0;
      }
      case DCTRL('d'): {
        if (e->len == 0) return -1;  /* end of input */
        if (e->pos < e->len)         /* otherwise delete forwards */
          erase(e, e->pos, nextchar(e->buf, e->len, e->pos));
        break;
      }
      case 127: case DCTRL('h'): {  /* backspace */
        if (e->pos > 0) erase(e, prevchar(e->buf, e->pos), e->pos);
        break;
      }
      case DCTRL('a'): e->pos = 0; break;
      case DCTRL('e'): e->pos = e->len; break;
      case DCTRL('b'): e->pos = prevchar(e->buf, e->pos); break;
      case DCTRL('f'): e->pos = nextchar(e->buf, e->len, e->pos); break;
      case DCTRL('k'): erase(e, e->pos, e->len); break;
      case DCTRL('u'): erase(e, 0, e->pos); break;
      case DCTRL('w'): erase(e, wordstart(e), e->pos); break;
      case DCTRL('l'): {  /* clear the screen, keep the line */
        if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) < 0) { }
        break;
      }
      case '\t': complete(e); continue;  /* 'complete' refreshes itself */
      case DCTRL('p'): case DCTRL('n'): {
        int dir = (c == DCTRL('p')) ? 1 : -1;
        if (histlen == 0) break;
        e->hindex += dir;
        if (e->hindex < 0) { e->hindex = 0; break; }
        if (e->hindex > histlen) { e->hindex = histlen; break; }
        if (e->hindex == 0) setline(e, "");
        else setline(e, hist[histlen - e->hindex]);
        break;
      }
      case 27: {  /* an escape sequence */
        char seq[3];
        if (read(STDIN_FILENO, seq, 1) != 1) break;
        if (read(STDIN_FILENO, seq + 1, 1) != 1) break;
        if (seq[0] == '[') {
          if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, seq + 2, 1) != 1) break;
            if (seq[2] == '~' && seq[1] == '3')       /* delete */
              erase(e, e->pos, nextchar(e->buf, e->len, e->pos));
            else if (seq[2] == '~' && seq[1] == '1')  /* home */
              e->pos = 0;
            else if (seq[2] == '~' && seq[1] == '4')  /* end */
              e->pos = e->len;
          }
          else switch (seq[1]) {
            case 'A': case 'B': {  /* up, down: browse the history */
              int dir = (seq[1] == 'A') ? 1 : -1;
              if (histlen == 0) break;
              e->hindex += dir;
              if (e->hindex < 0) { e->hindex = 0; break; }
              if (e->hindex > histlen) { e->hindex = histlen; break; }
              if (e->hindex == 0) setline(e, "");
              else setline(e, hist[histlen - e->hindex]);
              break;
            }
            case 'C': e->pos = nextchar(e->buf, e->len, e->pos); break;
            case 'D': e->pos = prevchar(e->buf, e->pos); break;
            case 'H': e->pos = 0; break;
            case 'F': e->pos = e->len; break;
          }
        }
        else if (seq[0] == 'O') {  /* some terminals send these for home/end */
          if (seq[1] == 'H') e->pos = 0;
          else if (seq[1] == 'F') e->pos = e->len;
        }
        break;
      }
      default: {
        if ((unsigned char)c < 32) break;  /* ignore other control codes */
        if (insert(e, &c, 1) != 0) break;
      }
    }
    refresh(e);
  }
}


char *diluvium_readline (lua_State *L, const char *prompt) {
  char buf[DLINE_MAXLINE];
  EditState e;
  char *line;
  if (!isatty(STDIN_FILENO))
    return readplain(NULL);  /* piped input takes no prompt */
  if (enterraw() == -1)
    return readplain(prompt);
  e.buf = buf;
  e.cap = sizeof(buf);
  e.len = e.pos = 0;
  e.buf[0] = '\0';
  e.prompt = (prompt != NULL) ? prompt : "";
  e.plen = strlen(e.prompt);
  e.cols = termcols();
  e.hindex = 0;
  e.L = L;
  if (editline(&e) == -1) {
    leaveraw();
    return NULL;
  }
  leaveraw();
  line = (char *)malloc(e.len + 1);
  if (line == NULL) return NULL;
  memcpy(line, e.buf, e.len + 1);
  return line;
}

#endif					/* } */
