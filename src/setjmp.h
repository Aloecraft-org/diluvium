/* setjmp.h — minimal stub for wasm32-unknown-unknown browser builds */
#ifndef _STUB_SETJMP_H
#define _STUB_SETJMP_H

typedef int jmp_buf[5];

int setjmp(jmp_buf env);
__attribute__((__noreturn__)) void longjmp(jmp_buf env, int val);

#endif