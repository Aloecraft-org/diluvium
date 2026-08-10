/*
** dhash_check.c
** SHA-256 conformance for src/dhash.c.
**
** doc/Messaging.md 10.5 makes this a security boundary, not a lookup
** convenience: a Proto is referenced by the hash of its stripped dump and
** restore "requires an exact match", so a collision means a snapshot naming one
** function and being handed another. A hand-written hash therefore has to be
** checked against the standard rather than against itself, and "it round-trips"
** proves nothing at all -- a hash that returned a constant would round-trip.
**
** Two kinds of case, because they catch different mistakes.
**
** The four published vectors (FIPS 180-4 / NIST CAVS) check the compression
** function and the message schedule. If a rotation constant or a K word is
** wrong, these fail and nothing else needs to run. The 1,000,000-byte one also
** forces the multi-block streaming path and a length that does not fit in a
** byte count anyone would test by hand.
**
** The generated table checks *padding and buffering*, which is where a
** from-scratch implementation actually goes wrong. Every length from 0 to 69
** covers all of: an empty message, a partial block, the 55/56/57 boundary where
** the length field stops fitting and a second block is needed, a message that
** exactly fills a block, and the first byte past it. Each length is hashed three
** ways -- one shot, one byte at a time, and split at every possible point -- so
** any state carried wrongly across an 'update' boundary shows up as a
** disagreement between the three even where the digest itself is not published.
** The expected digests came from Python's hashlib, i.e. from OpenSSL, not from
** this implementation.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dhash.h"


static int failures = 0;
static int checks = 0;

static void expect (const char *what, const unsigned char *digest,
                    const char *want) {
  char hex[DILUVIUM_SHA256_HEX];
  checks++;
  diluvium_sha256_hex(digest, hex);
  if (strcmp(hex, want) != 0) {
    printf("[FAIL] %s\n  got  %s\n  want %s\n", what, hex, want);
    failures++;
  }
}


/* The published vectors. */
static void nist_vectors (void) {
  static const struct { const char *msg; size_t len; const char *hex; } v[] = {
    { "", 0,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
    { "abc", 3,
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
    { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    { NULL, 0, NULL }
  };
  int i;
  unsigned char d[DILUVIUM_SHA256_SIZE];
  for (i = 0; v[i].hex != NULL; i++) {
    char what[96];
    snprintf(what, sizeof(what), "NIST vector, %lu bytes",
             (unsigned long)v[i].len);
    diluvium_sha256(v[i].msg, v[i].len, d);
    expect(what, d, v[i].hex);
  }
  /* One million 'a'. Heap-allocated rather than static so a build with a small
     BSS budget still runs it. */
  {
    char *big = (char *)malloc(1000000);
    if (big == NULL) {
      printf("[FAIL] out of memory for the 1e6 vector\n");
      failures++; checks++;
      return;
    }
    memset(big, 'a', 1000000);
    diluvium_sha256(big, 1000000, d);
    expect("NIST vector, one million 'a'", d,
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(big);
  }
}


/*
** Padding and buffering, over every length that can behave differently.
** Digests from Python's hashlib.
*/
static const struct { int n; const char *hex; } lengths[] = {
  {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
  {1, "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"},
  {2, "5dde896887f6754c9b15bfe3a441ae4806df2fde94001311e08bf110622e0bbe"},
  {3, "cd2eb0837c9b4c962c22d2ff8b5441b7b45805887f051d39bf133b583baf6860"},
  {4, "2481a63c85a62cf889d2b149f1a52e985a9341750173fe01eff50cc27b5941b5"},
  {5, "eaf16bc07968e013f3f94ab1342472434a39fc3475f11cf341a6c3965974f8e9"},
  {6, "b7fb217694ae2d305e766608d250f797daa984e4ac4b5fa638a729be352f2fcd"},
  {7, "7b70d3ab4c7641542e1f158b458eeae7cfb7bdb815d4110cc6178bafcfdf43f8"},
  {8, "4cf0c1012276f46af31e44d2fbb03ae7af56f03c9996eb9452b99b3e6273698e"},
  {9, "a73add1aecea03a894ca65d99bc5e91d21b58cdb728343bbcb6cf6cf38b809f5"},
  {10, "fc11d6f28e59d3cc33c0b14ceb644bf0902ebd63d61218dffe9e7dac7c254542"},
  {11, "7668d2cd32a37eb45b51f002690d1b9a7c03f2da91791f96227ba9a61952fce8"},
  {12, "59ffe12a70df15109e0345955e3230a978f31ebc28d8fe3e42d306afb28b8e81"},
  {13, "ea156a2c16eb8f24fe98ec6a36b3cb23229dfdb192c100f49828704fed25e43e"},
  {14, "8108ed602688b70712b2f9f1330e11a2ed1de1d274cabe2f70004a1abd103601"},
  {15, "b670825c8f110a2eabfb68acbebf2497ca3c91d3ba8c532a5c213d46f5eaac21"},
  {16, "9c5729595fb287cc2a2256280448bbe706c0a866f0372515c7483de2af4e6532"},
  {17, "d04fd59f3d9a1fd424c47874ae1dab0dde54fa73ba474245bfac79279afe6df7"},
  {18, "6f3226f77f030743e85dc8075aa901b63270626845a5585466bad787f530ce8e"},
  {19, "0d0f234feb2d235c5eb111a6a2486e6a710b72e1775926f75855ff7c927345e3"},
  {20, "d4fc1db665446507dc51b0c9392dd9649291581bfe1b48e241b2b08032b3b647"},
  {21, "7799e51458c8540c1caa9d18e9678b83c4b28e781dd47c22af5d023a3054bea4"},
  {22, "ec4a4fcd915a7672085936bb477cc50561660015653ad41616c8419727e97ff1"},
  {23, "564b146b27c9ded7e5110f6c53a78b2da3ccc295fe418e2534feaabb1dd0c720"},
  {24, "a649dbb05965890c0ba745a26fd6d7a451de29dabc6cf5f2ba0704892fef9dee"},
  {25, "c28e601ce25724907a7af09c08052e5dcf5b7a249dc719e4b90d86f51007c33c"},
  {26, "18d2a9c4fd84ab056bc0b93ce3e7c3809d3e66d3f089e066c2c0c4e29edfeca7"},
  {27, "e490a3d7a3e809950fbf9bb5071c16541f4f87b62dcdf3c56ea391326e13f384"},
  {28, "f3b439399a805a21e826bbd4111ca4c4615e492934f53bbb2be042e7c97530cc"},
  {29, "1702245ac78581c3218750b9aa8c45403e5ac485bd41b428f79a60f7b11c379b"},
  {30, "666a596df2cf2181e69835c4f812f7c6117ecbe5136a3a6be2a07ef4aaccd343"},
  {31, "0f46e4b0802fee6fed599682a16287d0397699cfd742025482c086a70979e56a"},
  {32, "c62e4615bd39e222572f3a1bf7c2132ea1e65b17ec805047bd6b2842c593493f"},
  {33, "11ba55a3a7c1ee0f8eb8867dc40a62c67240eb4a5ea125ee5c383fe996b57cd6"},
  {34, "b0b43789f1857820775bdb3ab67a4dd824bee60034dc4233f5412eb20ff4a4b8"},
  {35, "efa79bd8bf70266b39e0ccd08d615c56df473b6faa88769554d0e33f9cab8259"},
  {36, "0e63ae2820af0d78b5c53a58ba608fcc47bf002b155f5ea2132ed7ca14a25199"},
  {37, "6ee29e87d96ee4fcb3baef255923aecee9d004982c32c84675843e8f751ff95a"},
  {38, "1af257cacf04c01bca12f8a71cb0fdd24eb3657eec4d98da36e9808292d5c26e"},
  {39, "c374e89c980c902ffbb6474ddd8902b3562e01c70b274aa32f34cd62f2e82b4c"},
  {40, "bd913ff68243d41b9611b2690dfbf2b0f6e42ea14536a98232af60e9f64ffdaa"},
  {41, "3164596df4fdd018b2c567ec8c03e79bd76f4e4ca2a3fd020b577300ff9d2aca"},
  {42, "2b2573d5ea0b352e24bebd015f3fe83693a5b81a6252cf811b65dcf6a5037def"},
  {43, "cc0b1c2c66f3bb9fd1a081c626ba1bef62f6f96441a43be15268523776ac26a1"},
  {44, "4105d609cdfcf79a2a9bbd37cfda4527edda249dec350908a8842d99369c0339"},
  {45, "15b526372a5ee9a39c8eff1d35476aa2460d88be13a299e9e88f64fa0e203477"},
  {46, "404e0d6b65bc5eac7177b4e67f32a7815d5fdbf593e3010f103ccc3c97b0ebd3"},
  {47, "b35f3728761a26a57ec3e6e948981ae70cdd24c8769c4d647aa333422a9c61ca"},
  {48, "23b3634e2751a892cddd80e42c0027949226cffdb231c42cba9361fce2a3021e"},
  {49, "55bb9823e68e05f71c105395b87283afc86e549e1f1ca4598b1cc7a6dd2d3ac8"},
  {50, "77cf12060d47183ea8c40345e7389e7e05cb0753cab374a5e74f9329815b4cb5"},
  {51, "3ecd502af72c4cf4414ffb5c7eb7bdf2471bb7defa6bab588f94f1cf68040f95"},
  {52, "f43f0afcb07eb0dde147ed2828a5bd838d54a6b943ab969b107f6fa8b7bca0c7"},
  {53, "8a04a39a6914f857bf2d4521caf08b098a5ae803e74ae16e52ca902886299ce1"},
  {54, "45f316e10b2c99abf374b22bda893cf3300d77263f1e272349ed414680522952"},
  {55, "d5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072"},
  {56, "04c26261370ee7541549d16dee320c723e3fd14671e66a099afe0a377c16888e"},
  {57, "ae14a2563ccf969d99aca69ce6bb74981f734bbf9f655f73b8f06db68cab5217"},
  {58, "5ef8b2e5e7a040f31df9cf316b81c92326c8db51ac29bb80d914ec9590d83e99"},
  {59, "6e20a1199bf58ddb8fadbe63725f1dd9de8440153af062ca56b70d99a22c306e"},
  {60, "42f2d97335669f86846b721a8aa58551a46564eaf6977e3b4836d4645e3e3ed2"},
  {61, "c508e75f24e25db94dff52c9564a51025f69510191cc7dc2cfe16a6cb4b29f53"},
  {62, "21210f9644c6fe6b4b76ed90bd48a9f88d1b20bd22f17f65b83e1b91d2a52c45"},
  {63, "75220b47218278e656f2013bb8f0c455a25eaf01e86c64924e9d48d89776d6f2"},
  {64, "7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c"},
  {65, "9537c5fdf120482f7d58d25e9ed583f52c02b4e304ea814db1633ad565aed7e9"},
  {66, "6eb879f1291c6ee7d5c619d27c7b5c9c3318f58a76cab873d5e30263e50c924f"},
  {67, "9a84c81db565ba10e6f23545f0c4a3c615c082c9cf4a0be53274d5ec7ede5024"},
  {68, "c79817d7383e27961c1c0e247201b41ac1fd5a152677757dd0958f616228f330"},
  {69, "d3fcd241ff53d52e0b64de52bfe9b08fd3cdccb263108f5963ad46a2207683fa"},
  {111, "5ba60613dba318e9ed9020301e5dc59c721c19d82862e4d03718708aa75d2bad"},
  {112, "87bf6e70ecc829aa717756ac6797b82de8b30fca1281ea1659df31949839fc6b"},
  {113, "b8983b329f09f5296bc9951e1ec7ad9174001229698acea751675a5cf4eee0ee"},
  {119, "000b48d4edf0fa7bee3c6236ecd2785baa5db4eeb8bb54341b029e0d9fa5fb0c"},
  {120, "13f05a0b594787f5ecd315edc96141bd3243203d1b7d4f0836f37308b276ba98"},
  {127, "70156a14adbabf98cff3a71c7084b417abf057a8efd27329ca36b7202c87d81f"},
  {128, "24da1b81d0b16df6428eee73c69fcb2a93c76bc6df706f0c6670fe6bfe800464"},
  {129, "0ec9eb33e74510bcdd1f2ea55206e82f21649c5c2becbf2b433eb475b34c01bd"},
  {255, "d22609da3ae3956ca4877056a8e580eed744a6f7d7cfa5b19dd88d52fcc0d435"},
  {256, "85e62acd750c4eb56b7b6a1d66dca5bfaac5f062608a1a893410d0288936c09a"},
  {1000, "44f8354494a5ba03ba1792a8d3e9c534c47a9181980fde7a3f44b06ef2ae7c7f"},
  {1024, "49abd65bbf7f7e40c7055093ed2e3fd75f2f602f2c5fcf955c213e3135eb03f7"},
  {-1, NULL}
};


static void padding_and_streaming (void) {
  char buf[1200];
  int i;
  memset(buf, 'x', sizeof(buf));
  for (i = 0; lengths[i].n >= 0; i++) {
    int n = lengths[i].n;
    unsigned char one[DILUVIUM_SHA256_SIZE];
    char what[96];

    snprintf(what, sizeof(what), "one shot, n=%d", n);
    diluvium_sha256(buf, (size_t)n, one);
    expect(what, one, lengths[i].hex);

    /* A byte at a time visits every value of 'nblock', which is the state a
       streaming hash gets wrong. */
    {
      diluvium_sha256_ctx s;
      unsigned char d[DILUVIUM_SHA256_SIZE];
      int j;
      diluvium_sha256_init(&s);
      for (j = 0; j < n; j++)
        diluvium_sha256_update(&s, buf + j, 1);
      diluvium_sha256_final(&s, d);
      snprintf(what, sizeof(what), "byte at a time, n=%d", n);
      expect(what, d, lengths[i].hex);
    }

    /* And split at every point, which catches a partial block copied with the
       wrong offset or length. Counted as one check so the total stays
       readable; the message names the split that failed. */
    {
      int split, bad = -1;
      for (split = 0; split <= n && bad < 0; split++) {
        diluvium_sha256_ctx s;
        unsigned char d[DILUVIUM_SHA256_SIZE];
        diluvium_sha256_init(&s);
        diluvium_sha256_update(&s, buf, (size_t)split);
        diluvium_sha256_update(&s, buf + split, (size_t)(n - split));
        diluvium_sha256_final(&s, d);
        if (memcmp(d, one, sizeof(d)) != 0)
          bad = split;
      }
      checks++;
      if (bad >= 0) {
        printf("[FAIL] n=%d split at %d disagrees with the one-shot digest\n",
               n, bad);
        failures++;
      }
    }
  }
}


static void hex_is_lowercase_and_terminated (void) {
  unsigned char d[DILUVIUM_SHA256_SIZE];
  char hex[DILUVIUM_SHA256_HEX];
  int i;
  for (i = 0; i < DILUVIUM_SHA256_SIZE; i++)
    d[i] = (unsigned char)(i * 8);
  memset(hex, 'Z', sizeof(hex));
  diluvium_sha256_hex(d, hex);
  checks++;
  if (strlen(hex) != DILUVIUM_SHA256_SIZE * 2) {
    printf("[FAIL] hex digest is %lu chars, wanted %d\n",
           (unsigned long)strlen(hex), DILUVIUM_SHA256_SIZE * 2);
    failures++;
  }
  checks++;
  for (i = 0; hex[i] != '\0'; i++) {
    if (!((hex[i] >= '0' && hex[i] <= '9') ||
          (hex[i] >= 'a' && hex[i] <= 'f'))) {
      printf("[FAIL] hex digest has a non-hex or uppercase char at %d\n", i);
      failures++;
      return;
    }
  }
}


int main (void) {
  printf("=== SHA-256 ===\n");
  nist_vectors();
  padding_and_streaming();
  hex_is_lowercase_and_terminated();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
