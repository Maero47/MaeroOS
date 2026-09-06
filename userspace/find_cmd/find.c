/*
 * find — traverse directories
 * Usage: find [PATH...] [-name GLOB] [-type f|d] [-print] [-maxdepth N]
 */
#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"
#include "../include/dirent.h"

/* Simple glob: only '*' wildcard */
static int glob_match(const char *pat, const char *str) {
    while (*pat && *str) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*str) {
                if (glob_match(pat, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pat == '?' || *pat == *str) {
            pat++; str++;
        } else {
            return 0;
        }
    }
    while (*pat == '*') pat++;
    return *pat == '\0' && *str == '\0';
}

static const char *g_name_pat = NULL;  /* -name */
static int g_type = 0;  /* 0=any, 1=file, 2=dir */
static int g_maxdepth = 1024;

static void find_dir(const char *base, int depth) {
    if (depth > g_maxdepth) return;

    DIR *d = opendir(base);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[512];
        if (strcmp(base, "/") == 0)
            snprintf(path, sizeof(path), "/%s", ent->d_name);
        else
            snprintf(path, sizeof(path), "%s/%s", base, ent->d_name);

        int is_dir = (ent->d_type == 4);  /* DT_DIR */

        /* Type filter */
        if (g_type == 1 && is_dir) goto recurse;
        if (g_type == 2 && !is_dir) goto recurse;

        /* Name filter */
        if (g_name_pat && !glob_match(g_name_pat, ent->d_name)) goto recurse;

        printf("%s\n", path);

recurse:
        if (is_dir) find_dir(path, depth + 1);
    }
    closedir(d);
}

int main(int argc, char *argv[]) {
    const char *roots[64];
    int nroots = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i+1 < argc) {
            g_name_pat = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i+1 < argc) {
            i++;
            if (argv[i][0] == 'f') g_type = 1;
            else if (argv[i][0] == 'd') g_type = 2;
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i+1 < argc) {
            g_maxdepth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-print") == 0) {
            /* default */
        } else if (argv[i][0] != '-') {
            if (nroots < 64) roots[nroots++] = argv[i];
        }
    }

    if (!nroots) { roots[0] = "."; nroots = 1; }

    for (int i = 0; i < nroots; i++) {
        printf("%s\n", roots[i]);
        find_dir(roots[i], 1);
    }
    return 0;
}
