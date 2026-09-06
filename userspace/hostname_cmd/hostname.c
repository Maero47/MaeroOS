/* hostname — print /etc/hostname. */
#include "../include/stdio.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"
int main(void) {
    char buf[64] = {0};
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) { int n = read(fd, buf, 63); close(fd); if (n > 0) buf[n]=0; }
    for (int i = 0; buf[i]; i++) if (buf[i]=='\n'){buf[i]=0;break;}
    printf("%s\n", buf[0] ? buf : "maeros");
    return 0;
}
