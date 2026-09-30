#pragma once

/* Linux i386 layout (64 bytes); memory figures are in units of mem_unit. */
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[8];
};

int sysinfo(struct sysinfo *info);
int get_nprocs(void);
int get_nprocs_conf(void);
