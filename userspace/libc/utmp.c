/* utmp/utmpx access (see include/utmpx.h): a file of fixed-size records,
 * read sequentially; pututxline replaces the record with the same ut_id (or
 * ut_line) or appends one.  Written with a whole-record pwrite under an
 * exclusive flock so concurrent logins do not interleave. */
#include "../include/utmpx.h"
#include "../include/utmp.h"
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/string.h"
#include "../include/time.h"
#include "../include/sys/file.h"
#include "../include/sys/time.h"

static char ut_path[64] = _PATH_UTMP;
static int ut_fd = -1;
static struct utmpx ut_rec;

int utmpxname(const char *file) {
    if (!file || strlen(file) >= sizeof(ut_path)) return -1;
    endutxent();
    strcpy(ut_path, file);
    return 0;
}

void setutxent(void) {
    if (ut_fd < 0) ut_fd = open(ut_path, O_RDONLY | O_CLOEXEC);
    if (ut_fd >= 0) lseek(ut_fd, 0, SEEK_SET);
}

void endutxent(void) {
    if (ut_fd >= 0) close(ut_fd);
    ut_fd = -1;
}

struct utmpx *getutxent(void) {
    if (ut_fd < 0) setutxent();
    if (ut_fd < 0) return 0;
    if (read(ut_fd, &ut_rec, sizeof(ut_rec)) != (ssize_t)sizeof(ut_rec)) return 0;
    return &ut_rec;
}

static int id_match(const struct utmpx *a, const struct utmpx *b) {
    switch (b->ut_type) {
    case RUN_LVL: case BOOT_TIME: case NEW_TIME: case OLD_TIME:
        return a->ut_type == b->ut_type;
    case INIT_PROCESS: case LOGIN_PROCESS: case USER_PROCESS: case DEAD_PROCESS:
        return (a->ut_type == INIT_PROCESS || a->ut_type == LOGIN_PROCESS ||
                a->ut_type == USER_PROCESS || a->ut_type == DEAD_PROCESS) &&
               !strncmp(a->ut_id, b->ut_id, sizeof(a->ut_id));
    }
    return 0;
}

struct utmpx *getutxid(const struct utmpx *id) {
    struct utmpx *u;
    while ((u = getutxent()))
        if (id_match(u, id)) return u;
    return 0;
}

struct utmpx *getutxline(const struct utmpx *line) {
    struct utmpx *u;
    while ((u = getutxent()))
        if ((u->ut_type == LOGIN_PROCESS || u->ut_type == USER_PROCESS) &&
            !strncmp(u->ut_line, line->ut_line, sizeof(u->ut_line)))
            return u;
    return 0;
}

struct utmpx *pututxline(const struct utmpx *ut) {
    static struct utmpx copy;
    int fd = open(ut_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    flock(fd, LOCK_EX);
    struct utmpx cur;
    off_t at = -1, off = 0, free_at = -1;
    while (read(fd, &cur, sizeof(cur)) == (ssize_t)sizeof(cur)) {
        if (id_match(&cur, ut)) { at = off; break; }
        if (free_at < 0 && (cur.ut_type == EMPTY || cur.ut_type == DEAD_PROCESS) &&
            ut->ut_type == USER_PROCESS && !ut->ut_id[0])
            free_at = off;
        off += (off_t)sizeof(cur);
    }
    if (at < 0) at = off;                     /* append */
    int ok = pwrite(fd, ut, sizeof(*ut), at) == (ssize_t)sizeof(*ut);
    flock(fd, LOCK_UN);
    close(fd);
    (void)free_at;
    if (!ok) return 0;
    copy = *ut;
    return &copy;
}

void updwtmpx(const char *file, const struct utmpx *ut) {
    int fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    flock(fd, LOCK_EX);
    write(fd, ut, sizeof(*ut));
    flock(fd, LOCK_UN);
    close(fd);
}

void logwtmp(const char *line, const char *name, const char *host) {
    struct utmpx u;
    struct timeval tv;
    memset(&u, 0, sizeof(u));
    u.ut_type = name && name[0] ? USER_PROCESS : DEAD_PROCESS;
    u.ut_pid = getpid();
    if (line) strncpy(u.ut_line, line, sizeof(u.ut_line));
    if (name) strncpy(u.ut_user, name, sizeof(u.ut_user));
    if (host) strncpy(u.ut_host, host, sizeof(u.ut_host));
    gettimeofday(&tv, 0);
    u.ut_tv.tv_sec = (int)tv.tv_sec;
    u.ut_tv.tv_usec = (int)tv.tv_usec;
    updwtmpx(_PATH_WTMP, &u);
}
