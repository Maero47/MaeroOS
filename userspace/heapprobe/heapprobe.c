#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/sys/wait.h"
#include "../include/sys/socket.h"
#include "../include/sys/un.h"
#include "../include/netinet/in.h"

/*
 * heapprobe [rounds]: drive the allocation-heavy syscalls (fork/exit/wait,
 * pipes, AF_UNIX stream connections, UDP sockets, file open/close) in a loop
 * and ask the kernel for a kprof dump (syscall 503) before and after.  Every
 * round is balanced, so the dump's "heap live=" figure must come back to the
 * baseline taken after a warm-up round: that is the kernel-heap leak check
 * (tools/heap_stress.py compares the two).
 */

static void kprof_mark(void) {
    __asm__ volatile("int $0x80" :: "a"(503) : "memory");
}

static int one_round(int i) {
    int p[2];
    char buf[64];

    if (pipe(p) < 0) { printf("heapprobe: pipe failed\n"); return 1; }
    write(p[1], "pipe-data", 9);
    if (read(p[0], buf, sizeof buf) != 9) { printf("heapprobe: pipe read\n"); return 1; }
    close(p[0]); close(p[1]);

    int pid = fork();
    if (pid < 0) { printf("heapprobe: fork failed\n"); return 1; }
    if (pid == 0) _exit(i & 0x7f);
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) { printf("heapprobe: waitpid\n"); return 1; }

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls >= 0) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, "/tmp/heapprobe.sock");
        unlink(a.sun_path);
        if (bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 1) == 0) {
            int c = socket(AF_UNIX, SOCK_STREAM, 0);
            if (c >= 0 && connect(c, (struct sockaddr *)&a, sizeof a) == 0) {
                int s = accept(ls, NULL, NULL);
                if (s >= 0) {
                    write(c, "unix", 4);
                    read(s, buf, sizeof buf);
                    close(s);
                }
            }
            if (c >= 0) close(c);
        }
        close(ls);
        unlink(a.sun_path);
    }

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u >= 0) close(u);

    int fd = open("/tmp/heapprobe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, buf, 16); close(fd); }
    unlink("/tmp/heapprobe.txt");
    return 0;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 500;
    for (int i = 0; i < 20; i++) if (one_round(i)) return 1;   /* warm-up */
    printf("heapprobe: baseline\n");
    kprof_mark();
    for (int i = 0; i < rounds; i++) if (one_round(i)) return 1;
    printf("heapprobe: after %d rounds\n", rounds);
    kprof_mark();
    printf("heapprobe: done\n");
    return 0;
}
