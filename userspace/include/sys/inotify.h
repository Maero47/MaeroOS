#pragma once
#include <stdint.h>

struct inotify_event {
    int wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
};

#define IN_MODIFY      0x00000002
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF   0x00000800

int inotify_init(void);
int inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int inotify_rm_watch(int fd, int wd);
