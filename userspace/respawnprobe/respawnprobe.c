#include "../include/unistd.h"
#include "../include/fcntl.h"

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    write(fd, text, 5);
    close(fd);
}

int main(void) {
    /* Respawned: stay alive for svc/smoke-disk to see, asleep.  This used to
     * spin on sched_yield, which in every desktop boot put a runnable thread
     * next to the real work: ~420k context switches a second and no idle
     * time, which kprof then charged to "sched" and blamed on Firefox. */
    if (access("/home/root/respawn-alive", F_OK) == 0) {
        while (1) usleep(1000000);
    }

    write_file("/home/root/respawn-first", "first");
    write_file("/home/root/respawn-alive", "alive");
    return 0;
}
