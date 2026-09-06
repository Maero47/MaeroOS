#include "../include/errno.h"
#include "../include/stdio.h"
#include "../include/sys/socket.h"
#include "../include/netinet/in.h"
#include "../include/arpa/inet.h"
#include "../include/unistd.h"

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("sockprobe: udp socket failed errno=%d\n", errno);
        return 1;
    }

    struct sockaddr_in gw;
    gw.sin_family = AF_INET;
    gw.sin_port = htons(9);
    gw.sin_addr.s_addr = inet_addr("10.0.2.2");

    if (sendto(fd, "x", 1, 0, (struct sockaddr *)&gw, sizeof(gw)) != 1) {
        printf("sockprobe: udp sendto failed errno=%d\n", errno);
        close(fd);
        return 1;
    }

    char ch;
    if (recv(fd, &ch, 1, 0) != -1 || errno != EAGAIN) {
        printf("sockprobe: empty udp recv errno=%d\n", errno);
        close(fd);
        return 1;
    }

    close(fd);
    printf("sockprobe udp ok\n");
    return 0;
}
