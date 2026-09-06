/* libgreet.so.1 — a minimal external shared library for MaeroOS's loader.
 * Proves the kernel can open an arbitrary .so from disk, map its segments at
 * the correct file offsets, and resolve symbols ACROSS modules. */
#include "greet.h"

const char *greet_message(void) {
    return "GREET_OK";
}

int greet_add(int a, int b) {
    return a + b;
}
