/*
 * date — print current date/time (uptime since boot, in hh:mm:ss)
 */
#include "../include/unistd.h"
#include "../include/stdio.h"

int main(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long secs = tv.tv_sec;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    printf("MaeroOS boot+%02d:%02d:%02d\n", h, m, s);
    return 0;
}
