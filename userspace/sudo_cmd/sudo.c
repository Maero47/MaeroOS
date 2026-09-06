/* sudo — compatibility shim that runs the command via doas. */
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("usage: sudo <command> [args...]\n"); return 1; }
    const char *doas = access("/disk/doas", 1) == 0 ? "/disk/doas" : "/doas";
    /* `sudo -i` / `sudo -s` -> root shell */
    int start = 1;
    static char *nargv[64];
    int n = 0;
    nargv[n++] = (char *)doas;
    if (!strcmp(argv[1], "-i") || !strcmp(argv[1], "-s")) {
        const char *sh = access("/disk/shell", 1) == 0 ? "/disk/shell" : "/shell";
        nargv[n++] = (char *)sh;
        start = 2;
    }
    for (int i = start; i < argc && n < 62; i++) nargv[n++] = argv[i];
    nargv[n] = 0;
    execv(doas, nargv);
    printf("sudo: cannot exec doas\n");
    return 127;
}
