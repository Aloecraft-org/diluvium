/*
** dhost_fs.c
** The fs connector: the 'host:fs' capability family over a granted directory.
**
** The same scope discipline as sql (doc/BUILD7.md part 2), applied to plain
** files:
**
**   fs/read  {path}                 -> the bytes            host:fs/read
**   fs/write {path, data, append?}  -> {bytes = n}          host:fs/write
**
** The config grants a scope -- a directory -- and the program names its file
** within it. Unlike a database name, an fs path may descend: 'notes/a.txt'
** is legitimate structure. Containment is by resolution, not by pattern: the
** parent directory of the named file must 'realpath' to somewhere under the
** granted scope, and an existing file must itself resolve under it (so a
** symlink planted inside the scope cannot read or write through to the
** outside). A '..' component, an absolute path, or an embedded NUL is
** refused before resolution ever runs. Escape is DENIED, not clamped.
**
** Both directions are capped by 'max_bytes': a hostile read is a memory
** DoS, a hostile write fills the disk, and both refuse rather than
** truncate. 'fs/write' is wired only when the scope was granted
** access = "readwrite". Nothing is preallocated and no directory is
** created -- the program writes into structure that exists, which keeps a
** deployment's shape the deployment's decision.
**
** The trust boundary, stated rather than implied: containment is proven by
** resolution and then the file is reopened by name, so a LOCAL process
** with write access inside the scope could swap a checked path for a
** symlink between the two steps. That is the capability model's Profile A
** stance (doc/Capabilities.md: structuring for trusted programs, not a
** wall against a hostile co-tenant); the checks here stop a program's own
** traversal and every no-race escape, and a deployment that cannot trust
** the scope's other writers needs an OS-level jail around the host.
*/

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dmsgpack.h"
#include "dhost.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct dh_fs {
  char scope[PATH_MAX];                 /* realpath'd granted directory */
  size_t scopelen;
  long max_bytes;
  int readwrite;
} dh_fs;


static int fs_arg_str (const unsigned char *args, size_t argslen,
                       const char *key, const char **p, size_t *len) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  if (args == NULL)
    return 0;
  diluvium_mp_open(&c, args, argslen);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR)
    return 0;
  *p = t.p;
  *len = t.len;
  return 1;
}

static int fs_arg_bool (const unsigned char *args, size_t argslen,
                        const char *key) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  if (args == NULL)
    return 0;
  diluvium_mp_open(&c, args, argslen);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_BOOL)
    return 0;
  return t.b;
}

/*
** args.path -> the file's real location, or -1 with 'detail' and *status
** set. On success 'full' holds the joined path (for open) and the
** containment has been proven: the parent realpaths under the scope, and so
** does the file itself when it exists.
*/
static int resolve_path (dh_fs *s, const unsigned char *args, size_t argslen,
                         char *full, size_t fullcap, int *exists,
                         char *detail, size_t detailcap,
                         dh_call_status *status) {
  const char *path = NULL;
  size_t pathlen = 0;
  char parent[PATH_MAX];
  char resolved[PATH_MAX];
  struct stat sb;
  const char *seg;
  size_t seglen;
  const char *slash;

  if (!fs_arg_str(args, argslen, "path", &path, &pathlen) || pathlen == 0) {
    snprintf(detail, detailcap, "args.path must name a file within the "
                                "granted scope");
    *status = DH_CALL_ERROR;
    return -1;
  }
  if (pathlen >= PATH_MAX - 1 ||
      s->scopelen + 1 + pathlen + 1 > fullcap) {
    snprintf(detail, detailcap, "the path is longer than a path");
    *status = DH_CALL_ERROR;
    return -1;
  }
  if (memchr(path, '\0', pathlen) != NULL || path[0] == '/' ||
      memchr(path, '\\', pathlen) != NULL) {
    snprintf(detail, detailcap, "the path '%.*s' steps outside the granted "
                                "scope; a path is relative, inside it",
             (int)pathlen, path);
    *status = DH_CALL_DENIED;
    return -1;
  }
  /* No '.' or '..' as any component: resolution below would catch the
     escape, but a path that even *says* '..' is not a name, it is a
     traversal, and refusing it by shape gives the honest sentence. */
  seg = path;
  while (seg < path + pathlen) {
    slash = memchr(seg, '/', (size_t)(path + pathlen - seg));
    seglen = (slash != NULL) ? (size_t)(slash - seg)
                             : (size_t)(path + pathlen - seg);
    if (seglen == 0 ||
        (seglen == 1 && seg[0] == '.') ||
        (seglen == 2 && seg[0] == '.' && seg[1] == '.')) {
      snprintf(detail, detailcap, "the path '%.*s' steps outside the granted "
                                  "scope; a path is plain names, no '.' or "
                                  "'..'", (int)pathlen, path);
      *status = DH_CALL_DENIED;
      return -1;
    }
    seg += seglen + 1;
  }
  memcpy(full, s->scope, s->scopelen);
  full[s->scopelen] = '/';
  memcpy(full + s->scopelen + 1, path, pathlen);
  full[s->scopelen + 1 + pathlen] = '\0';
  /* The parent must exist and resolve under the scope -- structure is the
     deployment's, so nothing here creates it. */
  {
    const char *lastslash = strrchr(full, '/');
    size_t plen = (size_t)(lastslash - full);
    if (plen >= sizeof(parent)) {
      snprintf(detail, detailcap, "the path is longer than a path");
      *status = DH_CALL_ERROR;
      return -1;
    }
    memcpy(parent, full, plen);
    parent[plen] = '\0';
  }
  if (realpath(parent, resolved) == NULL) {
    snprintf(detail, detailcap, "the path's directory does not exist in the "
                                "granted scope (nothing here creates "
                                "structure)");
    *status = DH_CALL_ERROR;
    return -1;
  }
  if (!(strncmp(resolved, s->scope, s->scopelen) == 0 &&
        (resolved[s->scopelen] == '\0' || resolved[s->scopelen] == '/'))) {
    snprintf(detail, detailcap, "the path resolves outside the granted "
                                "scope");
    *status = DH_CALL_DENIED;
    return -1;
  }
  *exists = (lstat(full, &sb) == 0);
  if (*exists) {
    /* The name is there: it must itself resolve under the scope, which is
       what catches a symlink pointing out -- a dangling one fails realpath
       and is denied the same way rather than treated as creatable. */
    if (realpath(full, resolved) == NULL ||
        !(strncmp(resolved, s->scope, s->scopelen) == 0 &&
          resolved[s->scopelen] == '/')) {
      snprintf(detail, detailcap, "the path resolves outside the granted "
                                  "scope");
      *status = DH_CALL_DENIED;
      return -1;
    }
    /* The TARGET must be a regular file, checked through the link: a
       symlink to a FIFO would otherwise pass here and wedge the whole
       single-threaded host inside open(2). */
    if (stat(resolved, &sb) != 0 || !S_ISREG(sb.st_mode)) {
      snprintf(detail, detailcap, "the path is not a regular file");
      *status = DH_CALL_ERROR;
      return -1;
    }
  }
  return 0;
}

static dh_call_status conn_fs (void *ud, dvs_id id, int64_t tok,
                               const char *call,
                               const unsigned char *args, size_t argslen,
                               dh_buf *value, char *detail,
                               size_t detailcap) {
  dh_fs *s = (dh_fs *)ud;
  char full[PATH_MAX];
  int exists = 0;
  int is_write;
  dh_call_status status = DH_CALL_ERROR;
  (void)id;
  (void)tok;

  if (strcmp(call, "fs/read") == 0)
    is_write = 0;
  else if (strcmp(call, "fs/write") == 0)
    is_write = 1;
  else {
    snprintf(detail, detailcap, "the fs connector answers 'fs/read' and "
                                "'fs/write'; '%s' is neither", call);
    return DH_CALL_DENIED;
  }
  if (is_write && !s->readwrite) {
    snprintf(detail, detailcap, "this deployment grants read access "
                                "(config.connectors.fs.access \"read\"), so "
                                "'fs/write' is not wired");
    return DH_CALL_DENIED;
  }
  if (resolve_path(s, args, argslen, full, sizeof(full), &exists,
                   detail, detailcap, &status) != 0)
    return status;

  if (!is_write) {
    FILE *f;
    long size;
    char *buf;
    size_t got;
    if (!exists) {
      snprintf(detail, detailcap, "no such file in the granted scope");
      return DH_CALL_ERROR;
    }
    f = fopen(full, "rb");
    if (f == NULL) {
      snprintf(detail, detailcap, "the file would not open: %s",
               strerror(errno));
      return DH_CALL_ERROR;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
      snprintf(detail, detailcap, "the file would not size");
      fclose(f);
      return DH_CALL_ERROR;
    }
    if (size > s->max_bytes) {
      snprintf(detail, detailcap, "the file is bigger than this "
                                  "deployment's byte cap (%ld); refused "
                                  "rather than truncated", s->max_bytes);
      fclose(f);
      return DH_CALL_ERROR;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
      snprintf(detail, detailcap, "no memory for the file");
      fclose(f);
      return DH_CALL_ERROR;
    }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
      snprintf(detail, detailcap, "the file shrank while being read");
      free(buf);
      return DH_CALL_ERROR;
    }
    dh_lstr(value, buf, got);
    free(buf);
    return DH_CALL_OK;
  }

  {
    const char *data = NULL;
    size_t datalen = 0;
    int append;
    FILE *f;
    size_t wrote;
    if (!fs_arg_str(args, argslen, "data", &data, &datalen)) {
      snprintf(detail, detailcap, "args.data must be the bytes to write, as "
                                  "a string");
      return DH_CALL_ERROR;
    }
    if (datalen > (size_t)s->max_bytes) {
      snprintf(detail, detailcap, "the write is bigger than this "
                                  "deployment's byte cap (%ld); refused "
                                  "rather than truncated", s->max_bytes);
      return DH_CALL_ERROR;
    }
    append = fs_arg_bool(args, argslen, "append");
    f = fopen(full, append ? "ab" : "wb");
    if (f == NULL) {
      snprintf(detail, detailcap, "the file would not open for writing: %s",
               strerror(errno));
      return DH_CALL_ERROR;
    }
    wrote = fwrite(data, 1, datalen, f);
    if (fclose(f) != 0 || wrote != datalen) {
      snprintf(detail, detailcap, "the write did not complete");
      return DH_CALL_ERROR;
    }
    dh_map(value, 1);
    dh_str(value, "bytes");
    dh_uint(value, (uint64_t)wrote);
    return DH_CALL_OK;
  }
}

int dh_fs_open (dh_host *h, char *err, size_t errcap) {
  dh_fs *s;
  struct stat sb;
  char resolved[PATH_MAX];
  if (realpath(h->cfg.fs.scope, resolved) == NULL ||
      stat(resolved, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
    snprintf(err, errcap, "config.connectors.fs.scope '%s' does not resolve "
                          "to a directory", h->cfg.fs.scope);
    return -1;
  }
  s = (dh_fs *)malloc(sizeof(dh_fs));
  if (s == NULL) {
    snprintf(err, errcap, "no memory for the fs connector");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  strncpy(s->scope, resolved, sizeof(s->scope) - 1);
  s->scopelen = strlen(s->scope);
  s->max_bytes = h->cfg.fs.max_bytes;
  s->readwrite = h->cfg.fs.readwrite;
  if (dh_register(h, "fs", conn_fs, s) != 0) {
    snprintf(err, errcap, "could not register the fs connector");
    free(s);
    return -1;
  }
  h->fsctx = s;
  return 0;
}

void dh_fs_close (dh_host *h) {
  free(h->fsctx);
  h->fsctx = NULL;
}
