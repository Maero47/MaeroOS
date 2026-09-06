/* bbwrap — generic BusyBox applet launcher.  Copied to /disk/bin/<applet>;
 * execs "busybox <applet> <args>" based on its own argv[0] basename. */
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/stdio.h"

int main(int argc, char *argv[]) {
    const char *base = argv[0];
    for (const char *p = argv[0]; *p; p++) if (*p == '/') base = p + 1;
    const char *bb = access("/disk/bin/busybox", 1) == 0 ?
                     "/disk/bin/busybox" : "/bin/busybox";
    static char *nargv[66];
    int n = 0;
    nargv[n++] = (char *)bb;
    nargv[n++] = (char *)base;
    for (int i = 1; i < argc && n < 64; i++) nargv[n++] = argv[i];
    nargv[n] = 0;
    execv(bb, nargv);
    printf("%s: busybox not found\n", base);
    return 127;
}
