/*
 * dynprobe — a dynamically-linked (PIE, musl) probe for MaeroOS's ld.so path.
 *
 * If this prints DYNPROBE_OK, the kernel loaded the PIE + the musl dynamic
 * linker (/lib/ld-musl-i386.so.1), the interpreter relocated and resolved
 * libc symbols, and transferred control to main — the first rung of the
 * Firefox dynamic-linking ladder.  Built static-PIE-free (truly dynamic).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Exercise a couple of libc symbols resolved by the dynamic linker. */
    char buf[32];
    const char *who = getenv("USER");
    snprintf(buf, sizeof(buf), "%s", who ? who : "?");
    size_t n = strlen(buf);
    printf("DYNPROBE_OK user=%s len=%u\n", buf, (unsigned)n);
    fflush(stdout);
    return 0;
}
