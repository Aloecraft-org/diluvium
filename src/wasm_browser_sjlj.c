/*
** wasm_browser_sjlj.c
** setjmp/longjmp runtime for the wasm exception-handling proposal.
**
** VENDORED, verbatim but for this header, from wasi-libc
** (libc-top-half/musl/src/setjmp/wasm32/rt.c), MIT-licensed. It is the
** three functions LLVM's -mllvm -wasm-enable-sjlj lowering calls, which
** turn setjmp/longjmp -- and so Lua's pcall/error -- into a wasm 'throw'
** and 'try_table' rather than a trap. The wasi-sdk's own wasi-libc already
** contains this; Debian's does not, so the browser build (which links
** Debian's) carries its own copy. Do not edit; re-vendor from upstream.
**
** Upstream: https://github.com/WebAssembly/wasi-libc
** Reference: https://github.com/llvm/llvm-project/pull/84137
** SPDX-License-Identifier: MIT
*/

#include <stddef.h>
#include <stdint.h>

void __wasm_setjmp(void *env, uint32_t label, void *func_invocation_id);
uint32_t __wasm_setjmp_test(void *env, void *func_invocation_id);
void __wasm_longjmp(void *env, int val);

/* jmp_buf must be large enough and aligned to hold this. On wasm32 that is
   16 bytes; the guest's jmp_buf (src/wasm-shim/setjmp.h, int[5] = 20) is. */
struct jmp_buf_impl {
        void *func_invocation_id;
        uint32_t label;
        struct arg {
                void *env;
                int val;
        } arg;
};

void
__wasm_setjmp(void *env, uint32_t label, void *func_invocation_id)
{
        struct jmp_buf_impl *jb = env;
        if (label == 0) { /* ABI contract */
                __builtin_trap();
        }
        if (func_invocation_id == NULL) { /* sanity check */
                __builtin_trap();
        }
        jb->func_invocation_id = func_invocation_id;
        jb->label = label;
}

uint32_t
__wasm_setjmp_test(void *env, void *func_invocation_id)
{
        struct jmp_buf_impl *jb = env;
        if (jb->label == 0) { /* ABI contract */
                __builtin_trap();
        }
        if (func_invocation_id == NULL) { /* sanity check */
                __builtin_trap();
        }
        if (jb->func_invocation_id == func_invocation_id) {
                return jb->label;
        }
        return 0;
}

void
__wasm_longjmp(void *env, int val)
{
        struct jmp_buf_impl *jb = env;
        struct arg *arg = &jb->arg;
        /* C says longjmp cannot make setjmp return 0; 0 becomes 1. */
        if (val == 0) {
                val = 1;
        }
        arg->env = env;
        arg->val = val;
        __builtin_wasm_throw(1, arg); /* 1 == C_LONGJMP */
}

/* LLVM 22+ requires the tag symbol to be defined here; earlier clang provides
   it. Inert on the clang this builds with today, present for when it moves. */
#if __clang_major__ >= 22
__asm__(".globl __c_longjmp\n"
#if defined(__wasm32__)
        ".tagtype __c_longjmp i32\n"
#elif defined(__wasm64__)
        ".tagtype __c_longjmp i64\n"
#else
#error "Unsupported Wasm architecture"
#endif
        "__c_longjmp:\n");
#endif
