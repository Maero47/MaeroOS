#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/fcntl.h"
#include "../include/pthread.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/sys/mman.h"
#include "../include/sys/wait.h"

/* MaeroOS shared-memory syscalls */
#define SYS_SHM_CREATE 500
#define SYS_SHM_MAP    501
#define SYS_SHM_UNMAP  502
#define SYS_SHM_CHMOD  506

#define PG 4096

/* Plain global: must stay copy-on-write private across fork. */
static int g_private = 111;

static int fails;
#define CHECK(cond, ...) do {                                   \
        if (!(cond)) { printf("shmprobe: " __VA_ARGS__); printf("\n"); fails++; } \
    } while (0)

static int shm_create(int n)            { return syscall1(SYS_SHM_CREATE, n); }
static int shm_map(int id)              { return syscall1(SYS_SHM_MAP, id); }
static int shm_unmap(int id)            { return syscall1(SYS_SHM_UNMAP, id); }
static int shm_chmod(int id, int mode)  { return syscall2(SYS_SHM_CHMOD, id, mode); }

/* The real mmap/munmap (the in-tree libc's mmap is a malloc stand-in): old
 * mmap(90) takes its six arguments in a block. */
static void *k_mmap(void *addr, unsigned len, int prot, int flags) {
    unsigned a[6] = { (unsigned)addr, len, (unsigned)prot, (unsigned)flags,
                      (unsigned)-1, 0 };
    int r = syscall1(90, (int)a);
    return (r < 0 && r > -4096) ? MAP_FAILED : (void *)r;
}
static int k_munmap(void *addr, unsigned len) {
    return syscall2(91, (int)addr, (int)len);
}

/* Free physical memory in pages, from /proc/meminfo's MemFree (kB). */
static long free_pages(void) {
    char buf[256];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "MemFree:");
    if (!p) return -1;
    p += 8;
    while (*p == ' ') p++;
    return atol(p) / 4;
}

/* The original fork/COW/sharing checks. */
static void test_basic(void) {
    int id = shm_create(2);
    if (id < 0) { CHECK(0, "create failed (%d)", id); return; }
    int addr = shm_map(id);
    if (addr <= 0) { CHECK(0, "map failed (%d)", addr); return; }
    volatile unsigned *buf = (volatile unsigned *)addr;

    /* Fresh buffers must arrive zeroed. */
    CHECK(buf[0] == 0 && buf[1024] == 0, "buffer not zeroed");
    buf[0] = 0xAABBCCDDu;
    buf[1024] = 0x11223344u;   /* second page */

    int pid = fork();
    if (pid == 0) {
        /* Child: inherited mapping must show parent's writes... */
        if (buf[0] != 0xAABBCCDDu || buf[1024] != 0x11223344u) exit(1);
        /* ...child writes must be visible to the parent (no COW here)... */
        buf[0] = 0xC0FFEE00u;
        buf[1024] = 0xBEEF0000u;
        /* ...but ordinary memory must still be COW-private. */
        g_private = 222;
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(status == 0, "child read mismatch");
    CHECK(buf[0] == 0xC0FFEE00u && buf[1024] == 0xBEEF0000u,
          "shared write not visible (COW broke sharing)");
    CHECK(g_private == 111, "private memory shared across fork (COW broken)");

    CHECK(shm_unmap(id) == 0, "unmap failed");
    /* Object is destroyed once the last mapping is gone. */
    CHECK(shm_unmap(id) != 0 && shm_map(id) <= 0, "stale id not rejected");
}

/* munmap() of the attachment, then shm_unmap(): the frames must be released
 * exactly once, and not while another address space still maps them.  Run a
 * few times: the first round may still allocate page tables and heap. */
static void test_munmap_then_unmap(void) {
    const int n = 8;
    long base = -1;
    for (int round = 0; round < 3; round++) {
        long f0 = free_pages();
        int id = shm_create(n);
        if (id < 0) { CHECK(0, "munmap test: create %d", id); return; }
        int addr = shm_map(id);
        if (addr <= 0) { CHECK(0, "munmap test: map %d", addr); return; }
        memset((void *)addr, 0x5a, (size_t)n * PG);

        int to_child[2], to_parent[2];
        pipe(to_child); pipe(to_parent);
        int pid = fork();
        if (pid == 0) {
            char c;
            read(to_child[0], &c, 1);             /* parent detached */
            /* Our PTEs are the last mappings: dropping them with munmap
             * must not free the frames while the object (our record) is
             * alive, and then shm_unmap frees them once. */
            long before = free_pages();
            k_munmap((void *)addr, (size_t)n * PG);
            long mid = free_pages();
            int r = shm_unmap(id);
            long after = free_pages();
            char res = 0;
            if (mid != before) res |= 1;          /* freed while alive */
            if (r != 0) res |= 2;
            if (after - mid != n) res |= 4;       /* not freed exactly once */
            write(to_parent[1], &res, 1);
            exit(0);
        }
        /* The double-decref shape: our own munmap + shm_unmap. */
        CHECK(k_munmap((void *)addr, (size_t)n * PG) == 0, "munmap failed");
        CHECK(shm_unmap(id) == 0, "shm_unmap after munmap: not 0");
        write(to_child[1], "g", 1);
        char res = 0x7f;
        read(to_parent[0], &res, 1);
        int st;
        waitpid(pid, &st, 0);
        close(to_child[0]); close(to_child[1]);
        close(to_parent[0]); close(to_parent[1]);
        CHECK(!(res & 1), "munmap freed shm frames the object still holds (round %d)", round);
        CHECK(!(res & 2), "child shm_unmap after munmap failed");
        CHECK(!(res & 4), "shm frames not freed exactly once (round %d)", round);
        long f1 = free_pages();
        if (round > 0)
            CHECK(f1 == f0, "munmap+shm_unmap: free pages %ld before, %ld after", f0, f1);
        base = f0;
    }
    (void)base;
}

/* MAP_FIXED over the attachment, then shm_unmap(): the new mapping must stay
 * intact, and every frame must come back. */
static void test_map_fixed_over(void) {
    const int n = 8;
    for (int round = 0; round < 3; round++) {
        long f0 = free_pages();
        int id = shm_create(n);
        if (id < 0) { CHECK(0, "fixed test: create %d", id); return; }
        int addr = shm_map(id);
        if (addr <= 0) { CHECK(0, "fixed test: map %d", addr); return; }
        memset((void *)addr, 0x33, (size_t)n * PG);
        /* Replace the middle four pages. */
        char *mid = (char *)addr + 2 * PG;
        void *m = k_mmap(mid, 4 * PG, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
        CHECK(m == mid, "MAP_FIXED over the attachment failed");
        memset(mid, 0x77, 4 * PG);
        CHECK(shm_unmap(id) == 0, "shm_unmap after MAP_FIXED: not 0");
        /* The anonymous overlay is not the attachment's: still there. */
        int ok = 1;
        for (int i = 0; i < 4 * PG; i += 512) if ((unsigned char)mid[i] != 0x77) ok = 0;
        CHECK(ok, "shm_unmap wiped the mapping that replaced it (round %d)", round);
        k_munmap(mid, 4 * PG);
        long f1 = free_pages();
        if (round > 0)
            CHECK(f1 == f0, "MAP_FIXED+shm_unmap: free pages %ld before, %ld after", f0, f1);
    }
}

/* Access control: a non-owner may not map a 0600 object until the owner
 * widens the mode; only the owner (or root) may change it. */
static void test_access(void) {
    int id = shm_create(1);
    if (id < 0) { CHECK(0, "access test: create %d", id); return; }
    int to_child[2], to_parent[2];
    pipe(to_child); pipe(to_parent);
    int pid = fork();
    if (pid == 0) {
        int res = 0;
        if (setgid(1000) || setuid(1000)) exit(64);
        if (shm_map(id) != -13) res |= 1;           /* -EACCES */
        if (shm_chmod(id, 0666) != -1) res |= 2;    /* -EPERM */
        char c;
        write(to_parent[1], "t", 1);
        read(to_child[0], &c, 1);                   /* owner widened it */
        int a = shm_map(id);
        if (a <= 0) res |= 4;
        else if (shm_unmap(id) != 0) res |= 8;
        exit(res);
    }
    char c;
    read(to_parent[0], &c, 1);                      /* child tried at 0600 */
    int r = shm_chmod(id, 0666);
    write(to_child[1], "g", 1);
    int st = 0;
    waitpid(pid, &st, 0);
    int res = (st >> 8) & 0xff;
    CHECK(r == 0, "owner shm_chmod failed (%d)", r);
    CHECK(!(res & 1), "non-owner mapped a 0600 object (no EACCES)");
    CHECK(!(res & 2), "non-owner changed the mode (no EPERM)");
    CHECK(!(res & 4), "non-owner could not map after chmod 0666");
    CHECK(!(res & 8), "non-owner unmap failed");
    CHECK(shm_unmap(id) == 0, "creator could not give up an unmapped object");
    CHECK(shm_map(id) <= 0, "object survived its last reference");
    close(to_child[0]); close(to_child[1]);
    close(to_parent[0]); close(to_parent[1]);
}

/* An object that is never mapped goes when its creator exits (and more
 * objects than exist system-wide can be created that way). */
static void test_never_mapped(void) {
    long f0 = free_pages();
    for (int i = 0; i < 40; i++) {
        int pid = fork();
        if (pid == 0) {
            exit(shm_create(16) >= 0 ? 0 : 1);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (st != 0) { CHECK(0, "child %d could not create (slots leaked)", i); break; }
    }
    long f1 = free_pages();
    /* 40 x 16 pages would be 640 pages if they leaked. */
    CHECK(f0 - f1 < 64, "never-mapped objects leaked: %ld pages", f0 - f1);
    int id = shm_create(1);
    CHECK(id >= 0, "create after creator exits failed (%d)", id);
    if (id >= 0) CHECK(shm_unmap(id) == 0, "explicit destroy of an unmapped object failed");
}

/* Mapping from a non-leader thread must use the process's layout (never land
 * on a live mapping) and belong to the process, not to the thread. */
static volatile int t_go, t_addr;
static int t_id;
static void *mapper(void *arg) {
    (void)arg;
    while (!t_go) { }
    t_addr = shm_map(t_id);
    return 0;
}

static void test_thread_map(void) {
    pthread_t th;
    t_id = shm_create(4);
    if (t_id < 0) { CHECK(0, "thread test: create %d", t_id); return; }
    pthread_create(&th, 0, mapper, 0);
    /* Mapped after the thread was created: a stale per-thread cursor sits
     * below it. */
    char *r = k_mmap(0, 16 * PG, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS);
    CHECK(r != MAP_FAILED, "mmap failed");
    memset(r, 0x42, 16 * PG);
    /* ...and so is an attachment of the main thread's. */
    int id2 = shm_create(4);
    int a2 = id2 >= 0 ? shm_map(id2) : -1;
    CHECK(a2 > 0, "main thread shm_map failed (%d)", a2);
    if (a2 > 0) memset((void *)a2, 0x24, 4 * PG);
    t_go = 1;
    pthread_join(th, 0);
    unsigned a = (unsigned)t_addr;
    CHECK(t_addr > 0, "thread shm_map failed (%d)", t_addr);
    CHECK(a + 4 * PG <= (unsigned)r || a >= (unsigned)r + 16 * PG,
          "thread attachment 0x%x landed on a live mapping at %p", a, (void *)r);
    CHECK(a2 <= 0 || a + 4 * PG <= (unsigned)a2 || a >= (unsigned)a2 + 4 * PG,
          "thread attachment 0x%x landed on the main thread's at 0x%x", a, (unsigned)a2);
    int ok = 1;
    for (int i = 0; i < 16 * PG; i += 256) if ((unsigned char)r[i] != 0x42) ok = 0;
    if (a2 > 0)
        for (int i = 0; i < 4 * PG; i += 256)
            if (((unsigned char *)a2)[i] != 0x24) ok = 0;
    CHECK(ok, "thread attachment overwrote the process's memory");
    if (a2 > 0) CHECK(shm_unmap(id2) == 0, "unmap of the main thread's attachment failed");
    /* The thread is gone; the attachment is the process's. */
    if (t_addr > 0) {
        volatile unsigned *p = (volatile unsigned *)a;
        p[0] = 0x12345678u;
        CHECK(p[0] == 0x12345678u, "attachment unusable after the thread exited");
        CHECK(shm_unmap(t_id) == 0, "main thread could not unmap the thread's attachment");
    }
    k_munmap(r, 16 * PG);
}

/* The kernel holds freed frames in a 512-frame FIFO quarantine before they
 * count as free again (mm/pmm.c), so frame accounting is exact only once the
 * quarantine is full: every free then releases exactly one older frame.  Fill
 * it with a batch of throwaway pages first. */
static void prime_free_count(void) {
    const unsigned n = 700;
    char *p = k_mmap(0, n * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (p == MAP_FAILED) return;
    for (unsigned i = 0; i < n; i++) p[i * PG] = 1;
    k_munmap(p, n * PG);
}

/* A non-root user's objects are capped at SHM_UID_MAX_PAGES (8192 for the
 * session + 2048 for one transient surface) in all: five 2048-page objects
 * fit, one more page is -ENOSPC, and dropping one makes room again.  Root
 * is not capped. */
#define QUOTA_OBJS 5
static void test_uid_quota(void) {
    int pid = fork();
    if (pid == 0) {
        int ids[QUOTA_OBJS], res = 0;
        if (setgid(1000) || setuid(1000)) exit(64);
        for (int i = 0; i < QUOTA_OBJS; i++) {
            ids[i] = shm_create(2048);
            if (ids[i] < 0) exit(1);           /* under the quota: must fit */
        }
        if (shm_create(1) != -28) res |= 2;    /* over it: ENOSPC */
        if (shm_unmap(ids[QUOTA_OBJS - 1]) != 0) res |= 4;
        ids[QUOTA_OBJS - 1] = shm_create(2048);
        if (ids[QUOTA_OBJS - 1] < 0) res |= 8; /* freed pages count again */
        for (int i = 0; i < QUOTA_OBJS; i++)
            if (ids[i] >= 0) shm_unmap(ids[i]);
        exit(res);
    }
    int st = -1;
    waitpid(pid, &st, 0);
    st = (st >> 8) & 0xff;
    CHECK(st != 64, "quota test: setuid failed");
    CHECK(st != 1, "quota test: 5 x 2048 pages did not fit under the quota");
    CHECK(!(st & 2), "quota test: create past the per-uid quota was not ENOSPC");
    CHECK(!(st & 4), "quota test: shm_unmap failed");
    CHECK(!(st & 8), "quota test: freed pages did not return to the quota");
    {
        int id = shm_create(2048);             /* root: not capped */
        CHECK(id >= 0, "root could not create a 2048-page object (%d)", id);
        if (id >= 0) shm_unmap(id);
    }
}

int main(int argc, char **argv) {
    /* shmprobe [-v] [test...]: -v names each test as it starts; naming
     * tests runs only those. */
    int v = argc > 1 && strcmp(argv[1], "-v") == 0;
    int first = v ? 2 : 1;
    prime_free_count();
    static const struct { const char *name; void (*fn)(void); } tests[] = {
        { "basic", test_basic },
        { "munmap-then-unmap", test_munmap_then_unmap },
        { "map-fixed-over", test_map_fixed_over },
        { "access", test_access },
        { "never-mapped", test_never_mapped },
        { "thread-map", test_thread_map },
        { "uid-quota", test_uid_quota },
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int want = first >= argc;
        for (int a = first; a < argc; a++)
            if (strcmp(argv[a], tests[i].name) == 0) want = 1;
        if (!want) continue;
        if (v) printf("shmprobe: %s\n", tests[i].name);
        tests[i].fn();
    }
    if (fails) {
        printf("shmprobe: %d check(s) FAILED\n", fails);
        return 1;
    }
    puts("shmprobe ok");
    return 0;
}
