/*
** dhash_check.c
** SHA-256 and SHA-1 conformance for src/dhash.c.
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
**
** SHA-1 gets the same two kinds of case. It is in the tree for wire-format
** interop only (see dhash.h), but a wrong interop digest is still a wrong
** answer on the wire, so it is held to the same vectors-and-boundaries bar.
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


/* ================================================================ SHA-1 */

/* Local hex: 'diluvium_sha256_hex' is digest-size-specific and SHA-1 ships
   no hex helper of its own (nothing in the tree wants one). */
static void expect1 (const char *what, const unsigned char *digest,
                     const char *want) {
  static const char HEX[] = "0123456789abcdef";
  char hex[DILUVIUM_SHA1_SIZE * 2 + 1];
  int i;
  checks++;
  for (i = 0; i < DILUVIUM_SHA1_SIZE; i++) {
    hex[i * 2] = HEX[(digest[i] >> 4) & 0xF];
    hex[i * 2 + 1] = HEX[digest[i] & 0xF];
  }
  hex[DILUVIUM_SHA1_SIZE * 2] = '\0';
  if (strcmp(hex, want) != 0) {
    printf("[FAIL] %s\n  got  %s\n  want %s\n", what, hex, want);
    failures++;
  }
}


/* The published vectors (FIPS 180-4 / NIST CAVS). */
static void sha1_nist_vectors (void) {
  static const struct { const char *msg; size_t len; const char *hex; } v[] = {
    { "", 0,
      "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
    { "abc", 3,
      "a9993e364706816aba3e25717850c26c9cd0d89d" },
    { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
      "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
    { NULL, 0, NULL }
  };
  int i;
  unsigned char d[DILUVIUM_SHA1_SIZE];
  for (i = 0; v[i].hex != NULL; i++) {
    char what[96];
    snprintf(what, sizeof(what), "SHA-1 NIST vector, %lu bytes",
             (unsigned long)v[i].len);
    diluvium_sha1(v[i].msg, v[i].len, d);
    expect1(what, d, v[i].hex);
  }
  {
    char *big = (char *)malloc(1000000);
    if (big == NULL) {
      printf("[FAIL] out of memory for the SHA-1 1e6 vector\n");
      failures++; checks++;
      return;
    }
    memset(big, 'a', 1000000);
    diluvium_sha1(big, 1000000, d);
    expect1("SHA-1 NIST vector, one million 'a'", d,
            "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    free(big);
  }
}


/* The same length set as the SHA-256 table: the padding boundaries are at
   the same offsets (64-byte block, length field from byte 56), so the same
   lengths exercise them. Digests from Python's hashlib. */
static const struct { int n; const char *hex; } sha1_lengths[] = {
  {0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
  {1, "11f6ad8ec52a2984abaafd7c3b516503785c2072"},
  {2, "dd7b7b74ea160e049dd128478e074ce47254bde8"},
  {3, "b60d121b438a380c343d5ec3c2037564b82ffef3"},
  {4, "4ad583af22c2e7d40c1c916b2920299155a46464"},
  {5, "9addbf544119efa4a64223b649750a510f0d463f"},
  {6, "018f4d7f06cb8626e1756452581373e05ae41c56"},
  {7, "2db6d21d365f544f7ca3bcfb443ac96898a7a069"},
  {8, "bcf22dfc6fb76b7366b1f1675baf2332a0e6a7ce"},
  {9, "70374248fd7129088fef42b8f568443f6dce3a48"},
  {10, "ff9ee043d85595eb255c05dfe32ece02a53efbb2"},
  {11, "c2b6ff6ac90ae4c7ba8118bf82133b587f6844d0"},
  {12, "49901d945ad6da0f0af47691f305daf994d9d2c9"},
  {13, "35bf59a8608e6056fee877d137c05081fc98eb11"},
  {14, "33da8d0e8af2efc260f01d8e5edfcc5c5aba44ad"},
  {15, "f29546c9b9b5056412af91317f83158a4f5f06d4"},
  {16, "a7a7c2e911a47b967d34b5a8807c040e9d167815"},
  {17, "3f0155e75563ab3adc0505000a86da5baa207d1f"},
  {18, "38e57225a610ee2a597024ae2b31867844938b26"},
  {19, "dce1f02ca7cc4b63ac43008b7a3ce96e702a0c24"},
  {20, "d02e53411e8cb4cd709778f173f7bc9a3455f8ed"},
  {21, "67f47aa04705d775ee067d6db7d3d1196802990f"},
  {22, "22d980c81eb878c4a7731e77f2633831979d51f6"},
  {23, "2acc6756e4aa393274ae109f91c4ecdf5153604d"},
  {24, "f7228ea6b178df32077280927f544cf46831a5e7"},
  {25, "05711f1306adf20998dbdddbf0962f7eef6325f1"},
  {26, "d7b54da3c1ed6623cdaaa638fd7d7fb6099c65fb"},
  {27, "6fc065b11399e0d9523527aa593107f9301ec1f5"},
  {28, "a99d79c8a2946d7c89c67521a13a917928ca1b58"},
  {29, "ede3079249cce9fa824a8bb1d95447c6ebcea620"},
  {30, "5da451e73b2773e53c1d46d6e45fd897838621d1"},
  {31, "a700b9df6265e0e1a44fef607bf7319f702ed7e9"},
  {32, "680cb4c5ec5d1bbfa592081dcc915e15b3cd9d3e"},
  {33, "60bbb3c88636ba22efaea7c521d6f4ca17c62342"},
  {34, "94f5615ced9f0626ed6f7effcf12bb883632b147"},
  {35, "a5804110fb8af48579cb1ddc951b802c5dfd82ce"},
  {36, "b43c42666504175b55714a8404ab1c30b1ab88c8"},
  {37, "de9fb0ece0aaa283ed2d48399152a1329898848b"},
  {38, "4dcc4124122dc4a033fbdf28ca174fecb8dc8210"},
  {39, "931293b3347b83ce52911c47277a612d7d92f99a"},
  {40, "47372a7b27569d25063df5cbbf7606f615a8ec2a"},
  {41, "9dc0da3613af850c5a018b0a88a5626fb8888e4e"},
  {42, "30edcc340339d64cf63263a983283272c5cfc6d2"},
  {43, "1fb1c5bf6f209b731cab1656dc2c1901ac3ddca1"},
  {44, "0b8bb2499ed501bb7fd61ffc4192c829242209d1"},
  {45, "fbddf2383576fd1e5a416f44852fb66b26771e09"},
  {46, "65b044cc017d6d9499628d20bde3d6f2b30aff3d"},
  {47, "f89d4936f190d205f17b588e0d61dc9e085fade6"},
  {48, "9ba3571eafaf6619487a5b53a2e98096669dbfc9"},
  {49, "79bede281ed797b1b8ec4ddd20ca5456d6e59b3a"},
  {50, "c3f0ee5d874bc080fa3b88bfb21d3cc888365bd0"},
  {51, "c83a7fbb4caf846b22c9fcf132f0f16603f46de4"},
  {52, "e79c680685886f80ab385a40ff182baf1c28c1a9"},
  {53, "477598ced08c849d7d894afcf48e9c2ad2b3842d"},
  {54, "31045e7bb077ff8d188a776b196b980388735dbb"},
  {55, "cef734ba81a024479e09eb5a75b6ddae62e6abf1"},
  {56, "901305367c259952f4e7af8323f480d59f81335b"},
  {57, "025ecbd5d70f8fb3c5457cd96bab13fda305dc59"},
  {58, "1fc8ec1c521db349501a72ad396e44bfade318c2"},
  {59, "af3526de3ee728ffd84f7381df8c29b09e3a088d"},
  {60, "06ced2e070e58c2c4ed9f2b8cb890f0c512ce60d"},
  {61, "5482c87d17cc9f29b9f5580d168a712708b8ea98"},
  {62, "ff5b5136336035a9f58c21d5da1e2a1d29c67943"},
  {63, "0ddc4e0cccd9a12850deb5abb0853a4425559fec"},
  {64, "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163"},
  {65, "78c741ddc482e4cdf8c474a0876347a0905b6233"},
  {66, "b6a70490805fc2410afe1e58313de63717fb5663"},
  {67, "40a5698504d8c2dbf707911450f557a30aad7b4c"},
  {68, "87ce4c6f0048c287dbfcf288c97f54b619480279"},
  {69, "a1e3aea3264dee086dc89f1dc9be46c58a8e0f84"},
  {111, "ebcb0b3e48c9ef45a6cea955e622bad8c29ff4e7"},
  {112, "27fa638e78d8524dae129c782bb9042f0caed9f9"},
  {113, "bcaa0a9b88b39daeaf734543336f73a36b2e20f7"},
  {119, "4300320394f7ee239bcdce7d3b8bcee173a0cd5c"},
  {120, "ceb2821639c4b6dcb10bce0e522ca2e608ce056d"},
  {127, "e463484d274607e1897d4099497cbf2aedcf8206"},
  {128, "150fa3fbdc899bd0b8f95a9fb6027f564d953762"},
  {129, "2699b675922cc84a9b0dfd926eb7f8211c78693d"},
  {255, "63aa1abaadbb5698f2527c5cfcce7becf0465f97"},
  {256, "53dab551701657356ed8b75653865a2e7a9c2f42"},
  {1000, "c3efa690fa3fdd2e2526853eed670538ea127638"},
  {1024, "d5a3c9bd7e746c98b4aea0e9194fb9555b3c22ad"},
  {-1, NULL}
};


static void sha1_padding_and_streaming (void) {
  char buf[1200];
  int i;
  memset(buf, 'x', sizeof(buf));
  for (i = 0; sha1_lengths[i].n >= 0; i++) {
    int n = sha1_lengths[i].n;
    unsigned char one[DILUVIUM_SHA1_SIZE];
    char what[96];

    snprintf(what, sizeof(what), "SHA-1 one shot, n=%d", n);
    diluvium_sha1(buf, (size_t)n, one);
    expect1(what, one, sha1_lengths[i].hex);

    {
      diluvium_sha1_ctx s;
      unsigned char d[DILUVIUM_SHA1_SIZE];
      int j;
      diluvium_sha1_init(&s);
      for (j = 0; j < n; j++)
        diluvium_sha1_update(&s, buf + j, 1);
      diluvium_sha1_final(&s, d);
      snprintf(what, sizeof(what), "SHA-1 byte at a time, n=%d", n);
      expect1(what, d, sha1_lengths[i].hex);
    }

    {
      int split, bad = -1;
      for (split = 0; split <= n && bad < 0; split++) {
        diluvium_sha1_ctx s;
        unsigned char d[DILUVIUM_SHA1_SIZE];
        diluvium_sha1_init(&s);
        diluvium_sha1_update(&s, buf, (size_t)split);
        diluvium_sha1_update(&s, buf + split, (size_t)(n - split));
        diluvium_sha1_final(&s, d);
        if (memcmp(d, one, sizeof(d)) != 0)
          bad = split;
      }
      checks++;
      if (bad >= 0) {
        printf("[FAIL] SHA-1 n=%d split at %d disagrees with the one-shot "
               "digest\n", n, bad);
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
  printf("=== SHA-1 ===\n");
  sha1_nist_vectors();
  sha1_padding_and_streaming();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
