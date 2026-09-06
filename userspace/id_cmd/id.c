/*
 * id — print uid/gid
 */
#include "../include/unistd.h"
#include "../include/stdio.h"

int main(void) {
    int uid = getuid();
    int gid = getgid();
    printf("uid=%d gid=%d\n", uid, gid);
    return 0;
}
