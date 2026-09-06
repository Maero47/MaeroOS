/* wget — minimal HTTP downloader built on httpget. */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"
#include "../include/sys/wait.h"

int main(int argc, char *argv[]) {
    const char *url = 0, *out = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else url = argv[i];
    }
    if (!url) { printf("usage: wget [-O file] http://host[:port]/path\n"); return 1; }

    const char *p = url;
    if (!strncmp(p, "http://", 7)) p += 7;
    char host[128]; int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < 127) host[hi++] = *p++;
    host[hi] = 0;
    int port = 80;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port*10 + (*p++ - '0'); }
    const char *path = (*p == '/') ? p : "/";

    char defname[128];
    if (!out) {
        const char *base = path;
        for (const char *q = path; *q; q++) if (*q == '/') base = q + 1;
        snprintf(defname, sizeof(defname), "%s", (base && *base) ? base : "index.html");
        out = defname;
    }
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    const char *hg = access("/disk/httpget", 1) == 0 ? "/disk/httpget" : "/httpget";

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("wget: cannot create %s\n", out); return 1; }
    int pid = fork();
    if (pid == 0) {
        dup2(fd, 1);
        char *av[] = { (char *)hg, host, portstr, (char *)path, 0 };
        execv(hg, av);
        _exit(127);
    }
    close(fd);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    printf("wget: saved %s\n", out);
    return 0;
}
