#include "../include/unistd.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/dirent.h"
#include "../include/sys/stat.h"

static void print_entry(const char *name, unsigned char dtype) {
    /* Print type indicator */
    if (dtype == DT_DIR) {
        write(1, "\033[1;34m", 7);  /* bold blue for dirs */
        write(1, name, strlen(name));
        write(1, "/\033[0m", 5);
    } else {
        write(1, name, strlen(name));
    }
    write(1, "\n", 1);
}

int main(int argc, char *argv[]) {
    const char *path = (argc > 1) ? argv[1] : ".";

    /* Resolve "." to actual cwd */
    char cwd[256];
    if (path[0] != '/') {
        getcwd_syscall(cwd, sizeof(cwd));
        if (strcmp(path, ".") != 0) {
            int clen = strlen(cwd);
            if (cwd[clen-1] != '/') { cwd[clen] = '/'; clen++; }
            strncpy(cwd + clen, path, 255 - clen);
        }
        path = cwd;
    }

    DIR *d = opendir(path);
    if (!d) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != (struct dirent *)0) {
        /* Skip . and .. */
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        print_entry(de->d_name, de->d_type);
    }

    closedir(d);
    return 0;
}
