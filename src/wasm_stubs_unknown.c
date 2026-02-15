/* wasm_stubs.c — browser stubs for wasm32-unknown-unknown
 * 
 * Provides minimal C library surface so Lua compiles and runs
 * basic scripts. File I/O returns errors. OS functions are no-ops.
 * malloc/realloc/free come from the Rust side (dlmalloc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned int uint32_t;

/* ---- Forward decls for functions provided by Rust / linker ---- */
int system(const char *c) { return -1; }
FILE *tmpfile(void) { return NULL; }
char *tmpnam(char *s) { return NULL; }

lua_State *global_L = NULL;

__attribute__((export_name("init_lua"))) void init_lua()
{
    if (global_L == NULL)
    {
        // Ensure Lua doesn't buffer internally
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);

        global_L = luaL_newstate();
        luaL_openlibs(global_L);
    }
}

__attribute__((export_name("run_lua"))) int run_lua(const char *code)
{
    if (global_L == NULL)
        init_lua();

    int res = luaL_dostring(global_L, code);

    if (res != LUA_OK)
    {
        const char *err = lua_tostring(global_L, -1);
        printf("Error: %s\n", err);
        lua_pop(global_L, 1);
    }

    fflush(stdout);
    fflush(stderr);

    return res;
}

/* ================================================================
 * MEMORY ALLOCATOR — simple dlmalloc-style for browser wasm
 * 
 * We use a simple block allocator over a static buffer.
 * Each block has a header with its size.
 * ================================================================ */

#define HEAP_SIZE (8 * 1024 * 1024) /* 8 MB heap */
static unsigned char _heap[HEAP_SIZE];
static size_t _heap_pos = 0;
static void *_free_list = (void *)0;

/* Block header: stores the usable size of the allocation */
typedef struct { size_t size; } _block_hdr;
#define HDR_SIZE sizeof(_block_hdr)
#define ALIGN8(x) (((x) + 7) & ~7)

void *malloc(size_t size) {
    if (size == 0) return (void *)0;
    size = ALIGN8(size);

    /* Ensure block can hold a next pointer when freed */
    if (size < sizeof(void *)) size = sizeof(void *);

    /* Check free list for a first-fit block */
    void **curr = &_free_list;
    while (*curr) {
        _block_hdr *hdr = ((_block_hdr *)(*curr)) - 1;
        if (hdr->size >= size) {
            void *ptr = *curr;
            *curr = *(void **)ptr; /* Unlink from list */
            return ptr;
        }
        curr = (void **)*curr;
    }
    size_t needed = HDR_SIZE + size;
    if (_heap_pos + needed > HEAP_SIZE) return (void *)0;
    _block_hdr *hdr = (_block_hdr *)(&_heap[_heap_pos]);
    hdr->size = size;
    _heap_pos += needed;
    return (void *)(hdr + 1);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return (void *)0; }
    _block_hdr *hdr = ((_block_hdr *)ptr) - 1;
    size_t old_size = hdr->size;
    size = ALIGN8(size);
    if (size <= old_size) return ptr;
    void *new_ptr = malloc(size);
    if (!new_ptr) return (void *)0;
    unsigned char *dst = (unsigned char *)new_ptr;
    unsigned char *src = (unsigned char *)ptr;
    for (size_t i = 0; i < old_size; i++) dst[i] = src[i];
    free(ptr);  /* <-- return old block to free list */
    return new_ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    /* Store next pointer in the payload and push to list head */
    *(void **)ptr = _free_list;
    _free_list = ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        unsigned char *p = (unsigned char *)ptr;
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

/* ================================================================
 * STRING FUNCTIONS
 * ================================================================ */

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : (char *)0;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return (char *)0;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) { if (*s == *a) { found = 1; break; } a++; }
        if (!found) break;
        count++; s++;
    }
    return count;
}

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return (char *)0;
}

char *strerror(int errnum) {
    (void)errnum;
    return (char *)"error";
}

/* ================================================================
 * CTYPE FUNCTIONS
 * ================================================================ */

int isalnum(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isxdigit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int iscntrl(int c) {
    return (c >= 0 && c < 32) || c == 127;
}

int ispunct(int c) {
    return (c >= 33 && c <= 126) && !isalnum(c);
}

int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* ================================================================
 * STDIO — route print output via a JS-importable hook
 * ================================================================ */

/* Don't redefine FILE — use the one from stdio.h.
 * stdin/stdout/stderr are already declared in stdio.h.
 * We just need to provide the actual storage. */

/* We use a simple trick: we don't access FILE internals.
 * Instead we identify streams by pointer address. */
static int _get_fd(FILE *stream) {
    if (stream == stdout) return 1;
    if (stream == stderr) return 2;
    return 0; /* stdin or unknown */
}

__attribute__((import_module("env"), import_name("_diluvium_write")))
extern void _diluvium_write(int fd, const char *buf, int len);

static void write_out(int fd, const char *buf, size_t len) {
    _diluvium_write(fd, buf, (int)len);
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t total = size * nmemb;
    write_out(_get_fd(stream), (const char *)ptr, total);
    return nmemb;
}

int fputs(const char *s, FILE *stream) {
    size_t len = 0;
    const char *p = s;
    while (*p++) len++;
    write_out(_get_fd(stream), s, len);
    return 0;
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    write_out(_get_fd(stream), &ch, 1);
    return c;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    (void)stream; (void)fmt;
    return 0;
}

int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

int sprintf(char *buf, const char *fmt, ...) {
    if (buf) buf[0] = '\0';
    return 0;
}

int fflush(FILE *stream) { (void)stream; return 0; }
int feof(FILE *stream) { (void)stream; return 0; }
int ferror(FILE *stream) { (void)stream; return 0; }
void clearerr(FILE *stream) { (void)stream; }
int getc(FILE *stream) { (void)stream; return -1; }
int ungetc(int c, FILE *stream) { (void)c; (void)stream; return -1; }

char *fgets(char *s, int n, FILE *stream) {
    (void)s; (void)n; (void)stream;
    return (char *)0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}

long ftell(FILE *stream) { (void)stream; return -1; }

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    (void)path; (void)mode;
    return (FILE *)0;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    (void)path; (void)mode; (void)stream;
    return (FILE *)0;
}

int fclose(FILE *stream) { (void)stream; return 0; }

/* ================================================================
 * LOCALE
 * ================================================================ */

struct lconv {
    char *decimal_point;
    char *thousands_sep;
};

static struct lconv _lconv = { ".", "" };

struct lconv *localeconv(void) { return &_lconv; }

char *setlocale(int category, const char *locale) {
    (void)category; (void)locale;
    return (char *)"C";
}

/* ================================================================
 * TIME
 * ================================================================ */

// typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

time_t time(time_t *t) { if (t) *t = 0; return 0; }
clock_t clock(void) { return -1; }
double difftime(time_t t1, time_t t0) { return (double)(t1 - t0); }
time_t mktime(struct tm *tm) { (void)tm; return -1; }

static struct tm _zero_tm = {0};
struct tm *gmtime(const time_t *t) { (void)t; return &_zero_tm; }
struct tm *localtime(const time_t *t) { (void)t; return &_zero_tm; }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    (void)fmt; (void)tm;
    if (max) s[0] = '\0';
    return 0;
}

/* ================================================================
 * MATH
 * ================================================================ */

double frexp(double x, int *exp) {
    *exp = 0;
    return x; /* stub — Lua uses this for number formatting */
}

double strtod(const char *nptr, char **endptr) {
    /* Minimal strtod — Lua needs this for tonumber().
     * A real implementation is complex; this handles simple cases. */
    double result = 0.0;
    int sign = 1;
    const char *p = nptr;
    
    while (*p == ' ') p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }
    
    while (*p >= '0' && *p <= '9') {
        result = result * 10.0 + (*p - '0');
        p++;
    }
    if (*p == '.') {
        double frac = 0.1;
        p++;
        while (*p >= '0' && *p <= '9') {
            result += (*p - '0') * frac;
            frac *= 0.1;
            p++;
        }
    }
    if (endptr) *endptr = (char *)p;
    return result * sign;
}

/* ================================================================
 * OS / ENV
 * ================================================================ */

char *getenv(const char *name) { (void)name; return (char *)0; }
int remove(const char *path) { (void)path; return -1; }
int rename(const char *old, const char *new_) { (void)old; (void)new_; return -1; }

__attribute__((__noreturn__))
void exit(int status) { (void)status; __builtin_trap(); }

__attribute__((__noreturn__))
void abort(void) { __builtin_trap(); }

// int system(const char *cmd) { (void)cmd; return -1; }

/* ================================================================
 * SETJMP / LONGJMP — stub for browser builds
 * 
 * Lua uses setjmp/longjmp for error handling (lua_pcall etc).
 * Without a real implementation, longjmp will trap (abort).
 * This means Lua errors will crash instead of being caught.
 * Good enough for a demo; for production, use Lua's 
 * LUAI_THROW/LUAI_TRY override with C++ exceptions or 
 * a wasm-native approach.
 * ================================================================ */

typedef int jmp_buf[5];

int setjmp(jmp_buf env) {
    (void)env;
    return 0;
}

__attribute__((__noreturn__))
void longjmp(jmp_buf env, int val) {
    (void)env; (void)val;
    __builtin_trap();
}