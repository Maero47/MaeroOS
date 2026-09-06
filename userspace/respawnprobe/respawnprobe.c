#include "../include/unistd.h"
#include "../include/fcntl.h"

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    write(fd, text, 5);
    close(fd);
}

int main(void) {
    if (access("/home/root/respawn-alive", F_OK) == 0) {
        while (1) sched_yield();
    }

    write_file("/home/root/respawn-first", "first");
    write_file("/home/root/respawn-alive", "alive");
    return 0;
}
