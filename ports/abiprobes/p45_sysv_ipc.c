/*
 * P45 System V IPC: shared memory, semaphores, message queues.
 *
 * Linux: shmget/shmat share memory between a parent and a forked child
 * (shm_nattch counts both), a segment removed with IPC_RMID lives until the
 * last detach; semop blocks until another process posts, SEM_UNDO gives a
 * dead process's adjustment back, IPC_NOWAIT is EAGAIN and semtimedop times
 * out with EAGAIN; msgrcv selects by type (exact, lowest <= -type, any),
 * truncates only with MSG_NOERROR and blocks until a message arrives; a
 * removed set or queue wakes its sleepers with EIDRM; a SHM_RDONLY attach
 * cannot be made writable with mprotect (EACCES); a key names one object
 * (IPC_CREAT|IPC_EXCL on it again is EEXIST); a 0600 object refuses another
 * user (EACCES) and only its owner may remove it (EPERM).
 *
 * MaeroOS: every call was ENOSYS.
 */
#define PROBE_NAME "p45_sysv_ipc"
#include "probe.h"
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/wait.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int child_status(pid_t pid)
{
    int st;
    if (waitpid(pid, &st, 0) != pid) probe_fail("waitpid: %s", strerror(errno));
    if (!WIFEXITED(st)) probe_fail("child %d died of signal %d", (int)pid, WTERMSIG(st));
    return WEXITSTATUS(st);
}

static void test_shm(void)
{
    int id = shmget(IPC_PRIVATE, 65536, IPC_CREAT | 0600);
    if (id < 0) probe_fail("shmget: %s", strerror(errno));
    volatile int *p = shmat(id, NULL, 0);
    if (p == (void *)-1) probe_fail("shmat: %s", strerror(errno));
    if (p[0] != 0 || p[16383] != 0) probe_fail("a new segment is not zeroed");
    p[0] = 0x1234;
    struct shmid_ds ds;
    if (shmctl(id, IPC_STAT, &ds) != 0) probe_fail("IPC_STAT: %s", strerror(errno));
    if (ds.shm_segsz != 65536 || ds.shm_nattch != 1 || ds.shm_cpid != getpid())
        probe_fail("IPC_STAT: segsz %zu nattch %lu cpid %d", (size_t)ds.shm_segsz,
                   (unsigned long)ds.shm_nattch, (int)ds.shm_cpid);
    if ((ds.shm_perm.mode & 0777) != 0600 || ds.shm_perm.uid != geteuid())
        probe_fail("IPC_STAT: mode %o uid %u", ds.shm_perm.mode, (unsigned)ds.shm_perm.uid);

    pid_t pid = fork();
    if (pid == 0) {
        /* Inherited attachment, and a second one of the same segment. */
        if (p[0] != 0x1234) _exit(1);
        volatile int *q = shmat(id, NULL, 0);
        if (q == (void *)-1) _exit(2);
        struct shmid_ds d;
        if (shmctl(id, IPC_STAT, &d) != 0 || d.shm_nattch != 3) _exit(3);
        q[1] = 0x5678;
        if (shmdt((void *)q) != 0) _exit(4);
        _exit(0);
    }
    int r = child_status(pid);
    if (r) probe_fail("shm child step %d failed", r);
    if (p[1] != 0x5678) probe_fail("the child's write is not visible to the parent");
    if (shmctl(id, IPC_STAT, &ds) != 0 || ds.shm_nattch != 1)
        probe_fail("nattch after the child exited: %lu", (unsigned long)ds.shm_nattch);

    /* Read-only attach: a write must fault. */
    pid = fork();
    if (pid == 0) {
        volatile int *q = shmat(id, NULL, SHM_RDONLY);
        if (q == (void *)-1) _exit(2);
        if (q[0] != 0x1234) _exit(3);
        /* Nor can mprotect make it writable (Linux: EACCES). */
        if (mprotect((void *)q, 4096, PROT_READ | PROT_WRITE) == 0 || errno != EACCES) _exit(5);
        q[0] = 1;                          /* SIGSEGV */
        _exit(4);
    }
    int st;
    waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGSEGV)
        probe_fail("a write through a SHM_RDONLY attachment did not fault (status %x)", st);

    /* IPC_RMID while attached: still usable, gone after the last detach. */
    if (shmctl(id, IPC_RMID, NULL) != 0) probe_fail("IPC_RMID: %s", strerror(errno));
    p[2] = 7;
    if (p[2] != 7) probe_fail("the removed segment stopped working while attached");
    if (shmdt((void *)p) != 0) probe_fail("shmdt: %s", strerror(errno));
    if (shmctl(id, IPC_STAT, &ds) == 0 || errno != EINVAL)
        probe_fail("the removed segment outlived its last detach");
    if (shmdt((void *)p) == 0 || errno != EINVAL) probe_fail("shmdt of a detached address");
    if (shmget(IPC_PRIVATE, 0, 0600) >= 0 || errno != EINVAL) probe_fail("shmget size 0");
}

static void test_sem(void)
{
    int id = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    if (id < 0) probe_fail("semget: %s", strerror(errno));
    if (semctl(id, 0, GETVAL) != 0) probe_fail("a new semaphore is not 0");
    struct sembuf down = { 0, -1, 0 }, up = { 0, 1, 0 };

    /* A waiter blocks until another process posts. */
    pid_t pid = fork();
    if (pid == 0) {
        struct sembuf d = { 0, -1, 0 };
        _exit(semop(id, &d, 1) == 0 ? 0 : 1);
    }
    msleep(200);
    if (semctl(id, 0, GETNCNT) != 1) probe_fail("GETNCNT with one waiter: %d", semctl(id, 0, GETNCNT));
    if (semop(id, &up, 1) != 0) probe_fail("semop +1: %s", strerror(errno));
    if (child_status(pid) != 0) probe_fail("the waiter did not get the semaphore");
    if (semctl(id, 0, GETVAL) != 0) probe_fail("value after the hand-off: %d", semctl(id, 0, GETVAL));

    /* IPC_NOWAIT and a timed wait. */
    struct sembuf nw = { 0, -1, IPC_NOWAIT };
    if (semop(id, &nw, 1) == 0 || errno != EAGAIN) probe_fail("IPC_NOWAIT: want EAGAIN");
    struct timespec ts = { 0, 200000000L };
    if (semtimedop(id, &down, 1, &ts) == 0 || errno != EAGAIN)
        probe_fail("semtimedop: want EAGAIN after the timeout, got %s", strerror(errno));

    /* SEM_UNDO: a process that exits gives its adjustments back. */
    pid = fork();
    if (pid == 0) {
        struct sembuf u[2] = { { 0, 3, SEM_UNDO }, { 1, 1, SEM_UNDO } };
        _exit(semop(id, u, 2) == 0 ? 0 : 1);
    }
    if (child_status(pid) != 0) probe_fail("SEM_UNDO semop in the child failed");
    if (semctl(id, 0, GETVAL) != 0 || semctl(id, 1, GETVAL) != 0)
        probe_fail("SEM_UNDO not applied at exit: %d %d", semctl(id, 0, GETVAL),
                   semctl(id, 1, GETVAL));

    /* SETALL / GETALL, wait-for-zero. */
    unsigned short vals[2] = { 2, 5 }, got[2] = { 0, 0 };
    union semun arg;
    arg.array = vals;
    if (semctl(id, 0, SETALL, arg) != 0) probe_fail("SETALL: %s", strerror(errno));
    arg.array = got;
    if (semctl(id, 0, GETALL, arg) != 0 || got[0] != 2 || got[1] != 5)
        probe_fail("GETALL: %u %u", got[0], got[1]);
    struct sembuf z = { 0, 0, IPC_NOWAIT };
    if (semop(id, &z, 1) == 0 || errno != EAGAIN) probe_fail("wait-for-zero on 2 did not block");
    arg.val = 0;
    if (semctl(id, 0, SETVAL, arg) != 0) probe_fail("SETVAL: %s", strerror(errno));
    if (semop(id, &z, 1) != 0) probe_fail("wait-for-zero on 0: %s", strerror(errno));

    /* IPC_RMID wakes a sleeper with EIDRM. */
    pid = fork();
    if (pid == 0) {
        struct sembuf d = { 0, -1, 0 };
        int r = semop(id, &d, 1);
        _exit(r < 0 && errno == EIDRM ? 0 : 1);
    }
    msleep(200);
    if (semctl(id, 0, IPC_RMID) != 0) probe_fail("sem IPC_RMID: %s", strerror(errno));
    if (child_status(pid) != 0) probe_fail("a semop sleeper was not woken with EIDRM");
    if (semctl(id, 0, GETVAL) >= 0 || errno != EINVAL) probe_fail("removed set still answers");
}

struct msg { long type; char text[64]; };

static void test_msg(void)
{
    int id = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (id < 0) probe_fail("msgget: %s", strerror(errno));
    struct msg m;
    for (long t = 3; t >= 1; t--) {
        m.type = t;
        snprintf(m.text, sizeof m.text, "message %ld", t);
        if (msgsnd(id, &m, strlen(m.text) + 1, 0) != 0) probe_fail("msgsnd: %s", strerror(errno));
    }
    struct msqid_ds ds;
    if (msgctl(id, IPC_STAT, &ds) != 0 || ds.msg_qnum != 3 || ds.msg_lspid != getpid())
        probe_fail("IPC_STAT: qnum %lu", (unsigned long)ds.msg_qnum);
    ssize_t n = msgrcv(id, &m, sizeof m.text, 2, 0);
    if (n != 10 || m.type != 2 || strcmp(m.text, "message 2")) probe_fail("msgrcv type 2: %zd %ld", n, m.type);
    n = msgrcv(id, &m, sizeof m.text, -3, 0);
    if (n < 0 || m.type != 1) probe_fail("msgrcv type -3 took type %ld", m.type);
    n = msgrcv(id, &m, 4, 0, 0);
    if (n >= 0 || errno != E2BIG) probe_fail("a short buffer without MSG_NOERROR: want E2BIG");
    n = msgrcv(id, &m, 4, 0, MSG_NOERROR);
    if (n != 4 || m.type != 3 || memcmp(m.text, "mess", 4)) probe_fail("MSG_NOERROR: %zd", n);
    if (msgrcv(id, &m, sizeof m.text, 0, IPC_NOWAIT) >= 0 || errno != ENOMSG)
        probe_fail("empty queue with IPC_NOWAIT: want ENOMSG");

    pid_t pid = fork();
    if (pid == 0) {
        struct msg r;
        ssize_t k = msgrcv(id, &r, sizeof r.text, 9, 0);
        _exit(k == 3 && r.type == 9 && !strcmp(r.text, "hi") ? 0 : 1);
    }
    msleep(200);
    m.type = 9;
    strcpy(m.text, "hi");
    if (msgsnd(id, &m, 3, 0) != 0) probe_fail("msgsnd to a waiter: %s", strerror(errno));
    if (child_status(pid) != 0) probe_fail("a blocked msgrcv did not get its message");

    pid = fork();
    if (pid == 0) {
        struct msg r;
        _exit(msgrcv(id, &r, sizeof r.text, 0, 0) < 0 && errno == EIDRM ? 0 : 1);
    }
    msleep(200);
    if (msgctl(id, IPC_RMID, NULL) != 0) probe_fail("msg IPC_RMID: %s", strerror(errno));
    if (child_status(pid) != 0) probe_fail("a msgrcv sleeper was not woken with EIDRM");
}

static void test_keys_perms(void)
{
    key_t key = 0x4d450000 + (getpid() & 0xffff);
    int shm = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    int sem = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    int msq = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (shm < 0 || sem < 0 || msq < 0) probe_fail("keyed create: %s", strerror(errno));
    if (shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600) >= 0 || errno != EEXIST)
        probe_fail("IPC_EXCL on an existing key: want EEXIST");
    if (shmget(key, 4096, 0) != shm || semget(key, 1, 0) != sem || msgget(key, 0) != msq)
        probe_fail("lookup by key gave another id");
    if (shmget(key, 8192, 0) >= 0 || errno != EINVAL) probe_fail("bigger size than the segment: want EINVAL");
    if (semget(key, 2, 0) >= 0 || errno != EINVAL) probe_fail("more sems than the set: want EINVAL");

    if (geteuid() == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            if (setgid(65534) != 0 || setuid(65534) != 0) _exit(10);
            if (shmget(key, 4096, 0600) >= 0 || errno != EACCES) _exit(1);
            if (shmat(shm, NULL, 0) != (void *)-1 || errno != EACCES) _exit(2);
            struct sembuf up = { 0, 1, 0 };
            if (semop(sem, &up, 1) == 0 || errno != EACCES) _exit(3);
            struct msg m = { 1, "x" };
            if (msgsnd(msq, &m, 2, IPC_NOWAIT) == 0 || errno != EACCES) _exit(4);
            if (shmctl(shm, IPC_RMID, NULL) == 0 || errno != EPERM) _exit(5);
            if (semctl(sem, 0, IPC_RMID) == 0 || errno != EPERM) _exit(6);
            if (msgctl(msq, IPC_RMID, NULL) == 0 || errno != EPERM) _exit(7);
            _exit(0);
        }
        int r = child_status(pid);
        if (r == 10) probe_fail("setuid(65534) failed");
        if (r) probe_fail("another user got through a 0600 object's check (step %d)", r);
        /* Mode 0666 lets the same user in. */
        struct shmid_ds ds;
        shmctl(shm, IPC_STAT, &ds);
        ds.shm_perm.mode = 0666;
        if (shmctl(shm, IPC_SET, &ds) != 0) probe_fail("IPC_SET: %s", strerror(errno));
        pid = fork();
        if (pid == 0) {
            if (setgid(65534) != 0 || setuid(65534) != 0) _exit(10);
            void *a = shmat(shm, NULL, 0);
            _exit(a == (void *)-1 ? 1 : 0);
        }
        if (child_status(pid) != 0) probe_fail("mode 0666 after IPC_SET still refused");
    } else {
        probe_info("not root: the other-user checks are skipped");
    }
    shmctl(shm, IPC_RMID, NULL);
    semctl(sem, 0, IPC_RMID);
    msgctl(msq, IPC_RMID, NULL);
    if (shmget(key, 4096, 0) >= 0 || errno != ENOENT) probe_fail("a removed key is still found");
}

int main(void)
{
    probe_watchdog(60);
    int first = shmget(IPC_PRIVATE, 4096, 0600);
    if (first < 0 && errno == ENOSYS) probe_fail("shmget: ENOSYS");
    if (first >= 0) shmctl(first, IPC_RMID, NULL);
    test_shm();
    test_sem();
    test_msg();
    test_keys_perms();
    probe_pass();
}
