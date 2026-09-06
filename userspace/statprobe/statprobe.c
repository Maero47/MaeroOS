#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

int main(void) {
    const char *paths[] = { "/etc/resolv.conf", "/disk/etc", "/disk/wallpaper.ppm", "/disk" };
    struct stat st;
    for (int i = 0; i < 4; i++) {
        int r = stat(paths[i], &st);
        printf("stat %s -> r=%d mode=%o reg=%d dir=%d\n", paths[i], r,
               r == 0 ? (unsigned)st.st_mode : 0,
               r == 0 ? S_ISREG(st.st_mode) : -1,
               r == 0 ? S_ISDIR(st.st_mode) : -1);
    }
    DIR *d = opendir("/disk");
    struct dirent *e;
    while (d && (e = readdir(d)))
        printf("dirent %s d_type=%d\n", e->d_name, e->d_type);
    if (d) closedir(d);
    return 0;
}
