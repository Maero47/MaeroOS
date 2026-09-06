#pragma once

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

void *setmntent(const char *filename, const char *type);
struct mntent *getmntent(void *stream);
int endmntent(void *stream);
