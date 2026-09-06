#include <unistd.h>
#include <string.h>
#include <stdio.h>

extern char **environ;

int main(int argc, char *argv[]) {
    /* env [NAME=VALUE...] [CMD [ARGS...]]
     * Without CMD: print environment.
     * With CMD: exec it (we don't actually modify environ here for simplicity). */
    int i = 1;
    /* Skip any NAME=VALUE pairs — they would require putenv which we lack */
    while (i < argc && strchr(argv[i], '=') != NULL) i++;

    if (i >= argc) {
        /* Print environment */
        if (environ) {
            for (int j = 0; environ[j]; j++) {
                puts(environ[j]);
            }
        }
        return 0;
    }

    execve(argv[i], &argv[i], environ);
    fprintf(stderr, "env: %s: exec failed\n", argv[i]);
    return 127;
}
