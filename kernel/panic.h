#pragma once
#include <registers.h>

/* Print a kernel panic message + optional register dump, then halt forever */
void panic(const char *msg, registers_t *regs) __attribute__((noreturn));
