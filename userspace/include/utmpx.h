#pragma once
#include <sys/types.h>

/* utmp/wtmp records (Linux/glibc i386 layout, 384 bytes), read and written
 * by libc/utmp.c.  init clears /var/run/utmp at boot and records the boot,
 * login records a USER_PROCESS for each session, init marks it
 * DEAD_PROCESS when the session ends; /var/log/wtmp keeps all of them.  On
 * an initrd-only boot init mounts tmpfs over /var/run and /var/log. */

#define EMPTY          0
#define RUN_LVL        1
#define BOOT_TIME      2
#define NEW_TIME       3
#define OLD_TIME       4
#define INIT_PROCESS   5
#define LOGIN_PROCESS  6
#define USER_PROCESS   7
#define DEAD_PROCESS   8
#define ACCOUNTING     9

#define UT_LINESIZE    32
#define UT_NAMESIZE    32
#define UT_HOSTSIZE    256

#define _PATH_UTMP     "/var/run/utmp"
#define _PATH_WTMP     "/var/log/wtmp"
#define _PATH_UTMPX    _PATH_UTMP
#define _PATH_WTMPX    _PATH_WTMP

struct utmpx {
    short ut_type;
    short __ut_pad1;
    pid_t ut_pid;
    char  ut_line[UT_LINESIZE];
    char  ut_id[4];
    char  ut_user[UT_NAMESIZE];
    char  ut_host[UT_HOSTSIZE];
    struct { short __e_termination, __e_exit; } ut_exit;
    int   ut_session;
    struct { int tv_sec, tv_usec; } ut_tv;
    unsigned ut_addr_v6[4];
    char  __unused[20];
};

void setutxent(void);
void endutxent(void);
struct utmpx *getutxent(void);
struct utmpx *getutxid(const struct utmpx *);
struct utmpx *getutxline(const struct utmpx *);
struct utmpx *pututxline(const struct utmpx *);
int utmpxname(const char *);
void updwtmpx(const char *, const struct utmpx *);
