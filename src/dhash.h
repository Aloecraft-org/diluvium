/*
** dhash.h
** SHA-256.
**
** doc/Messaging.md 10.5 addresses a Proto by the hash of its stripped dump and
** says restore "requires an exact match". That makes the hash a security
** boundary rather than a lookup convenience: a collision means a snapshot
** naming one function and getting another, which is arbitrary code execution
** with the runtime's own cooperation. So this is SHA-256 and not one of the
** fast non-cryptographic hashes already in the tree -- 'luaS_hash' is seeded
** for table dispersion and makes no collision-resistance claim at all.
**
** The same function does the 10.6 runtime fingerprint, where the requirement is
** only that different builds differ. One implementation for both, because a
** second one would be a second thing to get wrong.
**
** Self-contained: no Lua types, nothing from the core. That is deliberate --
** the compiler (luac) needs it too, and it must not drag the runtime in.
*/

#ifndef dhash_h
#define dhash_h

#include <stddef.h>


#define DILUVIUM_SHA256_SIZE	32   /* raw digest bytes */
#define DILUVIUM_SHA256_HEX	65   /* hex digest + terminator */


typedef struct diluvium_sha256_ctx {
  unsigned long h[8];              /* state, 32 bits used per word */
  unsigned char block[64];         /* partial block */
  size_t nblock;                   /* bytes in 'block' */
  unsigned long long total;        /* message length in bytes */
} diluvium_sha256_ctx;


void diluvium_sha256_init (diluvium_sha256_ctx *s);
void diluvium_sha256_update (diluvium_sha256_ctx *s, const void *data, size_t len);

/* Finishes 's' and writes DILUVIUM_SHA256_SIZE bytes. 's' is spent after. */
void diluvium_sha256_final (diluvium_sha256_ctx *s, unsigned char *out);

/* One-shot. 'out' takes DILUVIUM_SHA256_SIZE bytes. */
void diluvium_sha256 (const void *data, size_t len, unsigned char *out);

/*
** Lowercase hex of a digest, NUL-terminated. 'out' takes DILUVIUM_SHA256_HEX
** bytes. Snapshot headers carry hex rather than raw bytes so that a mismatch
** can be reported in a message a person can compare by eye -- which is the
** whole diagnostic when a snapshot refuses to load.
*/
void diluvium_sha256_hex (const unsigned char *digest, char *out);

#endif
