#include "../include/unistd.h"
#include "../include/stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { write(2, "usage: sleep SECONDS\n", 21); exit(1); }
    int secs = atoi(argv[1]);
    if (secs <= 0) exit(0);
    struct timespec ts;
    ts.tv_sec  = secs;
    ts.tv_nsec = 0;
    nanosleep(&ts, (struct timespec *)0);
    exit(0);
    return 0;
}
