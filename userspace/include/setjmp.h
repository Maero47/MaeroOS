#pragma once

typedef int jmp_buf[32];
typedef int sigjmp_buf[32];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
int sigsetjmp(sigjmp_buf env, int savesigs);
void siglongjmp(sigjmp_buf env, int val);
