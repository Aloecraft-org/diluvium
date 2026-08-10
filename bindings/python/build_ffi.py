"""Build the CFFI extension.

API mode, not ABI mode: the extension compiles the amalgamation, so the module
either builds against the real headers or fails at build time. ABI mode would
`dlopen` a shared object and defer every mismatch to the first call, which for a
binding whose whole job is to refuse a version mismatch is the wrong trade.

The published wheels build against the prebuilt archives from the release mirror;
this path -- compiling `src/` in place -- is what the repository's own tests use,
so a test never depends on a release having happened.
"""

from pathlib import Path

from cffi import FFI

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

ffi = FFI()

# Deliberately a transcription of the parts of dv.h that CFFI can parse: no
# comments, no macros, no #include. Keeping it literal means a reader can diff it
# against the header, and `dv_check.c` is what proves the header itself works.
ffi.cdef(
    """
    typedef struct dv_instance dv_instance;
    typedef uint32_t dv_queue_id;
    typedef int dv_status;

    typedef struct dv_config {
        uint32_t abi_version;
        uint32_t flags;
        size_t memory_limit;
        void *reserved;
    } dv_config;

    typedef struct dv_queue_info {
        uint32_t capacity;
        uint32_t len;
        uint8_t enabled;
        uint8_t exported;
        uint8_t direction;
        uint8_t on_full;
    } dv_queue_info;

    typedef struct dv_waitset {
        uint32_t n;
        dv_queue_id ids[32];
        int64_t timeout_ms;
        uint8_t for_write;
    } dv_waitset;

    uint32_t dv_abi_version(void);
    const char *dv_status_name(dv_status s);

    dv_instance *dv_new(const dv_config *cfg);
    void dv_free(dv_instance *inst);
    dv_status dv_load(dv_instance *inst, const uint8_t *code, size_t len,
                      const char *name);
    const char *dv_last_error(dv_instance *inst);

    dv_queue_id dv_queue_lookup(dv_instance *inst, const char *name);
    dv_status dv_queue_state(dv_instance *inst, dv_queue_id id,
                             dv_queue_info *out);
    dv_status dv_queue_push(dv_instance *inst, dv_queue_id id,
                            const uint8_t *msgpack, size_t len);
    dv_status dv_queue_pop(dv_instance *inst, dv_queue_id id, uint8_t *buf,
                           size_t cap, size_t *out_len);
    dv_status dv_queue_peek(dv_instance *inst, dv_queue_id id,
                            const uint8_t **ptr, size_t *out_len);
    void dv_queue_release(dv_instance *inst, dv_queue_id id);

    dv_status dv_run(dv_instance *inst, dv_waitset *out_waitset);
    dv_status dv_resume(dv_instance *inst, dv_queue_id fired);
    dv_status dv_waitset_get(dv_instance *inst, dv_waitset *out);

    void dv_set_notify(dv_instance *inst,
                       void (*cb)(void *ud, dv_queue_id id), void *ud);
    """
)

import sys

define_macros = [("MAKE_LIB", "1")]
libraries = ["m"]
if sys.platform.startswith("linux"):
    define_macros.append(("LUA_USE_LINUX", "1"))
    libraries.append("dl")
elif sys.platform == "darwin":
    define_macros.append(("LUA_USE_MACOSX", "1"))

ffi.set_source(
    "diluvium._dv",
    '#include "dv.h"',
    sources=[str(SRC / "onelua.c")],
    include_dirs=[str(SRC)],
    define_macros=define_macros,
    libraries=libraries,
    extra_compile_args=["-O2", "-std=c99"],
)

if __name__ == "__main__":
    # Into src/, beside __init__.py, so an in-place build is importable without
    # installing. A wheel build overrides this via the build backend.
    ffi.compile(tmpdir=str(Path(__file__).parent / "src"), verbose=False)
