#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/syscall.h"
#include "../include/unistd.h"
#include "../include/sys/wait.h"

/* MaeroOS shared-memory syscalls */
#define SYS_SHM_CREATE 500
#define SYS_SHM_MAP    501
#define SYS_SHM_UNMAP  502

/* Plain global: must stay copy-on-write private across fork. */
static int g_private = 111;

int main(void) {
    int id = syscall1(SYS_SHM_CREATE, 2);
    if (id < 0) {
        printf("shmprobe: create failed (%d)\n", id);
        return 1;
    }

    int addr = syscall1(SYS_SHM_MAP, id);
    if (addr <= 0) {
        printf("shmprobe: map failed (%d)\n", addr);
        return 1;
    }
    volatile unsigned *buf = (volatile unsigned *)addr;

    /* Fresh buffers must arrive zeroed. */
    if (buf[0] != 0 || buf[1024] != 0) {
        printf("shmprobe: buffer not zeroed\n");
        return 1;
    }

    buf[0] = 0xAABBCCDDu;
    buf[1024] = 0x11223344u;   /* second page */

    int pid = fork();
    if (pid == 0) {
        /* Child: inherited mapping must show parent's writes... */
        if (buf[0] != 0xAABBCCDDu || buf[1024] != 0x11223344u) {
            printf("shmprobe: child read mismatch\n");
            exit(1);
        }
        /* ...child writes must be visible to the parent (no COW here)... */
        buf[0] = 0xC0FFEE00u;
        buf[1024] = 0xBEEF0000u;
        /* ...but ordinary memory must still be COW-private. */
        g_private = 222;
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0) {
        printf("shmprobe: child failed\n");
        return 1;
    }
    if (buf[0] != 0xC0FFEE00u || buf[1024] != 0xBEEF0000u) {
        printf("shmprobe: shared write not visible (COW broke sharing)\n");
        return 1;
    }
    if (g_private != 111) {
        printf("shmprobe: private memory shared across fork (COW broken)\n");
        return 1;
    }

    if (syscall1(SYS_SHM_UNMAP, id) != 0) {
        printf("shmprobe: unmap failed\n");
        return 1;
    }
    /* Object is destroyed once the last mapping is gone. */
    if (syscall1(SYS_SHM_UNMAP, id) == 0 || syscall1(SYS_SHM_MAP, id) > 0) {
        printf("shmprobe: stale id not rejected\n");
        return 1;
    }

    puts("shmprobe ok");
    return 0;
}
