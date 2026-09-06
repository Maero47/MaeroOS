/* df — filesystem table.  MaeroOS has a fixed mount set; report it. */
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"

int main(void) {
    printf("%-12s %10s %10s %10s %5s %s\n",
           "Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");
    /* initrd root (ramfs) */
    printf("%-12s %10s %10s %10s %5s %s\n",
           "initrd", "-", "-", "-", "-", "/");
    /* tmpfs */
    printf("%-12s %10s %10s %10s %5s %s\n",
           "tmpfs", "-", "-", "-", "-", "/tmp");
    /* ext2 disk (report size if accessible) */
    if (access("/disk/etc", 0) == 0)
        printf("%-12s %10d %10s %10s %5s %s\n",
               "/dev/hda", 65536, "?", "?", "-", "/disk");
    return 0;
}
