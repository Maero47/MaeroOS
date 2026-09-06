#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: dirname PATH\n");
        return 1;
    }
    char buf[512];
    strncpy(buf, argv[1], 511); buf[511] = '\0';
    int len = strlen(buf);

    /* Strip trailing slashes */
    while (len > 1 && buf[len-1] == '/') { buf[--len] = '\0'; }

    /* Find last slash */
    int last = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (buf[i] == '/') { last = i; break; }
    }

    if (last < 0) { puts("."); return 0; }
    if (last == 0) { puts("/"); return 0; }
    buf[last] = '\0';
    puts(buf);
    return 0;
}
