/* dynprobe2 — a dynamically-linked PIE that calls into the EXTERNAL shared
 * library libgreet.so.1.  If it prints GREET_OK, the dynamic linker located,
 * mapped, and relocated a real external .so (cross-module PLT/GOT). */
#include <stdio.h>
#include "greet.h"

int main(void) {
    const char *m = greet_message();   /* resolved from libgreet.so.1 */
    int s = greet_add(40, 2);          /* cross-module call */
    printf("%s sum=%d\n", m, s);
    fflush(stdout);
    return 0;
}
