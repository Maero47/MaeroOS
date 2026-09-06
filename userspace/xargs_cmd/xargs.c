/*
 * xargs — build and execute commands from stdin
 * Usage: xargs [-n N] COMMAND [ARGS...]
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

#define MAX_ARGS 256
#define WORD_MAX 512

static int run_cmd(char *argv[], int argc) {
    if (!argc) return 0;
    int pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        execve(argv[0], argv, NULL);
        printf("xargs: %s: not found\n", argv[0]);
        exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

int main(int argc, char *argv[]) {
    int n_per = 0;  /* -n N: max args per invocation */
    int ai = 1;
    if (ai < argc && strcmp(argv[ai], "-n") == 0 && ai + 1 < argc) {
        n_per = atoi(argv[ai + 1]);
        ai += 2;
    }

    /* base command */
    char *base[MAX_ARGS];
    int base_n = 0;
    for (int i = ai; i < argc && base_n < MAX_ARGS - 1; i++)
        base[base_n++] = argv[i];

    /* read words from stdin */
    char words[MAX_ARGS][WORD_MAX];
    int nwords = 0;
    char buf[256];
    int n;
    static char wbuf[WORD_MAX];
    int wi = 0;

    while ((n = (int)read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == ' ' || c == '\t' || c == '\n') {
                if (wi > 0) {
                    wbuf[wi] = '\0';
                    if (nwords < MAX_ARGS - base_n - 1) {
                        strncpy(words[nwords++], wbuf, WORD_MAX - 1);
                    }
                    wi = 0;
                }
            } else {
                if (wi < WORD_MAX - 1) wbuf[wi++] = c;
            }
        }
    }
    if (wi > 0) {
        wbuf[wi] = '\0';
        if (nwords < MAX_ARGS - base_n - 1)
            strncpy(words[nwords++], wbuf, WORD_MAX - 1);
    }

    if (!nwords) return 0;

    int chunk = (n_per > 0) ? n_per : nwords;
    int ret = 0;
    for (int i = 0; i < nwords; i += chunk) {
        char *call[MAX_ARGS];
        int ci = 0;
        for (int j = 0; j < base_n; j++) call[ci++] = base[j];
        for (int j = i; j < nwords && j < i + chunk; j++)
            call[ci++] = words[j];
        call[ci] = NULL;
        int r = run_cmd(call, ci);
        if (r) ret = r;
    }
    return ret;
}
