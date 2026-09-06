/* du — summarize disk usage of a directory tree (KB). */
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/dirent.h"
#include "../include/sys/stat.h"
#include "../include/unistd.h"

static long du(const char *path, int top) {
    struct stat st;
    if (lstat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (st.st_size + 1023) / 1024;
    long total = 1;   /* dir entry itself ~1KB */
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[512];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            total += du(child, 0);
        }
        closedir(d);
    }
    if (top) printf("%ld\t%s\n", total, path);
    return total;
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";
    du(path, 1);
    return 0;
}
