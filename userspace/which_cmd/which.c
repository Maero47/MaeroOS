/* which — locate a command on PATH. */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("usage: which <cmd>...\n"); return 1; }
    const char *path = getenv("PATH");
    if (!path) path = "/disk:/disk/bin:/:/bin";
    int rc = 0;
    for (int a = 1; a < argc; a++) {
        char *cmd = argv[a];
        if (strchr(cmd, '/')) {            /* explicit path */
            if (access(cmd, 1) == 0) { printf("%s\n", cmd); continue; }
            rc = 1; continue;
        }
        char buf[256]; const char *p = path; int found = 0;
        while (*p) {
            int i = 0;
            while (*p && *p != ':' && i < 200) buf[i++] = *p++;
            if (*p == ':') p++;
            buf[i] = 0;
            char full[300], norm[300];
            snprintf(full, sizeof(full), "%s/%s", buf, cmd);
            /* Collapse any run of '/' so a "/" PATH entry can't yield "//cmd". */
            int o = 0, prev_slash = 0;
            for (int k = 0; full[k] && o < (int)sizeof(norm) - 1; k++) {
                if (full[k] == '/') {
                    if (prev_slash) continue;
                    prev_slash = 1;
                } else {
                    prev_slash = 0;
                }
                norm[o++] = full[k];
            }
            norm[o] = 0;
            if (access(norm, 1) == 0) { printf("%s\n", norm); found = 1; break; }
        }
        if (!found) rc = 1;
    }
    return rc;
}
