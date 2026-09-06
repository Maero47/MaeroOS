#pragma once
#include <stdint.h>

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

struct dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct {
    int    fd;
    int    pos;
    char   buf[4096];
    int    buf_pos;
    int    buf_len;
} DIR;

DIR           *opendir(const char *path);
DIR           *fdopendir(int fd);
struct dirent *readdir(DIR *dirp);
int            closedir(DIR *dirp);
