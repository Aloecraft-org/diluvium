/*
** dhash.c
** SHA-256 (FIPS 180-4). See dhash.h for why this is here.
**
** Written against the standard rather than adapted from a reference
** implementation, so it has no licence to carry and no upstream to track. It is
** the textbook construction; the only thing worth commenting is the masking.
**
** 'unsigned long' is at least 32 bits everywhere C guarantees anything, and may
** be 64. So every arithmetic step masks to 32 bits explicitly instead of
** relying on the type to wrap. Using uint32_t would make that automatic, but
** this file is also compiled into luac under -std=c89 on the portable target,
** where <stdint.h> is not promised.
*/

#define dhash_c

#include "lprefix.h"

#include <string.h>

#include "dhash.h"


#define M32(x)		((x) & 0xFFFFFFFFUL)
#define ROR(x,n)	M32(M32(x) >> (n) | M32(x) << (32 - (n)))

#define CH(x,y,z)	(((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z)	(((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)	(ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define BSIG1(x)	(ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SSIG0(x)	(ROR(x, 7) ^ ROR(x, 18) ^ M32((x) >> 3))
#define SSIG1(x)	(ROR(x, 17) ^ ROR(x, 19) ^ M32((x) >> 10))


static const unsigned long sha_k[64] = {
  0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
  0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
  0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
  0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
  0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
  0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
  0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
  0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
  0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
  0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
  0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
  0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
  0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
  0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
  0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
  0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};


static void sha_block (diluvium_sha256_ctx *s, const unsigned char *p) {
  unsigned long w[64];
  unsigned long a, b, c, d, e, f, g, h;
  int t;
  for (t = 0; t < 16; t++) {
    w[t] = M32((unsigned long)p[t * 4] << 24 |
               (unsigned long)p[t * 4 + 1] << 16 |
               (unsigned long)p[t * 4 + 2] << 8 |
               (unsigned long)p[t * 4 + 3]);
  }
  for (t = 16; t < 64; t++)
    w[t] = M32(SSIG1(w[t - 2]) + w[t - 7] + SSIG0(w[t - 15]) + w[t - 16]);
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
  e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (t = 0; t < 64; t++) {
    unsigned long t1 = M32(h + BSIG1(e) + CH(e, f, g) + sha_k[t] + w[t]);
    unsigned long t2 = M32(BSIG0(a) + MAJ(a, b, c));
    h = g; g = f; f = e;
    e = M32(d + t1);
    d = c; c = b; b = a;
    a = M32(t1 + t2);
  }
  s->h[0] = M32(s->h[0] + a); s->h[1] = M32(s->h[1] + b);
  s->h[2] = M32(s->h[2] + c); s->h[3] = M32(s->h[3] + d);
  s->h[4] = M32(s->h[4] + e); s->h[5] = M32(s->h[5] + f);
  s->h[6] = M32(s->h[6] + g); s->h[7] = M32(s->h[7] + h);
}


void diluvium_sha256_init (diluvium_sha256_ctx *s) {
  s->h[0] = 0x6a09e667UL; s->h[1] = 0xbb67ae85UL;
  s->h[2] = 0x3c6ef372UL; s->h[3] = 0xa54ff53aUL;
  s->h[4] = 0x510e527fUL; s->h[5] = 0x9b05688cUL;
  s->h[6] = 0x1f83d9abUL; s->h[7] = 0x5be0cd19UL;
  s->nblock = 0;
  s->total = 0;
  memset(s->block, 0, sizeof(s->block));
}


void diluvium_sha256_update (diluvium_sha256_ctx *s, const void *data,
                             size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  s->total += (unsigned long long)len;
  if (s->nblock > 0) {  /* finish the partial block first */
    size_t want = 64 - s->nblock;
    size_t take = (len < want) ? len : want;
    memcpy(s->block + s->nblock, p, take);
    s->nblock += take;
    p += take;
    len -= take;
    if (s->nblock < 64)
      return;
    sha_block(s, s->block);
    s->nblock = 0;
  }
  while (len >= 64) {
    sha_block(s, p);
    p += 64;
    len -= 64;
  }
  if (len > 0) {
    memcpy(s->block, p, len);
    s->nblock = len;
  }
}


void diluvium_sha256_final (diluvium_sha256_ctx *s, unsigned char *out) {
  unsigned long long bits = s->total * 8;
  int i;
  s->block[s->nblock++] = 0x80;
  if (s->nblock > 56) {  /* no room for the length: pad this block out */
    memset(s->block + s->nblock, 0, 64 - s->nblock);
    sha_block(s, s->block);
    s->nblock = 0;
  }
  memset(s->block + s->nblock, 0, 56 - s->nblock);
  for (i = 0; i < 8; i++)
    s->block[56 + i] = (unsigned char)((bits >> (56 - i * 8)) & 0xFF);
  sha_block(s, s->block);
  for (i = 0; i < 8; i++) {
    out[i * 4]     = (unsigned char)((s->h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = (unsigned char)((s->h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = (unsigned char)((s->h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = (unsigned char)(s->h[i] & 0xFF);
  }
}


void diluvium_sha256 (const void *data, size_t len, unsigned char *out) {
  diluvium_sha256_ctx s;
  diluvium_sha256_init(&s);
  diluvium_sha256_update(&s, data, len);
  diluvium_sha256_final(&s, out);
}


void diluvium_sha256_hex (const unsigned char *digest, char *out) {
  static const char hex[] = "0123456789abcdef";
  int i;
  for (i = 0; i < DILUVIUM_SHA256_SIZE; i++) {
    out[i * 2] = hex[(digest[i] >> 4) & 0xF];
    out[i * 2 + 1] = hex[digest[i] & 0xF];
  }
  out[DILUVIUM_SHA256_SIZE * 2] = '\0';
}
