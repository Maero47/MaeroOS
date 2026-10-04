/*
 * P81 MaeroOS shm_size (syscall 509): a shm object's real size.
 *
 * The desktop maps a client's surface by shm id; the client also claims its
 * width and height.  The object's own size must bound what the compositor
 * reads, and the desktop must learn it from the kernel (as shmctl IPC_STAT
 * shm_segsz does for System V), not by parsing /proc/self/maps.
 *
 * Checks: size == npages * 4096; an unknown id is EINVAL; another user is
 * refused (EACCES) while the object is 0600, allowed once it is 0644.
 * MaeroOS only: on Linux the custom syscalls do not exist (SKIP).
 */
#define PROBE_NAME "p81_shm_size"
#include "probe.h"
#include <sys/wait.h>

int main(void)
{
    probe_watchdog(60);
    long id = syscall(500, 3);
    if (id < 0 && errno == ENOSYS) probe_skip("no MaeroOS shm syscalls (not MaeroOS)");
    if (id < 0) probe_fail("shm_create(3): %s", strerror(errno));
    long sz = syscall(509, id);
    if (sz != 3 * 4096)
        probe_fail("shm_size = %ld (%s), want %d", sz, sz < 0 ? strerror(errno) : "-", 3 * 4096);
    if (syscall(509, -1) != -1 || errno != EINVAL)
        probe_fail("shm_size(-1) is not EINVAL");
    if (getuid() != 0) probe_skip("needs root for the other-user part");

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && syscall(506, id, 0644) != 0)
            probe_fail("shm_chmod: %s", strerror(errno));
        pid_t pid = fork();
        if (pid == 0) {
            if (setgid(65534) || setuid(65534)) _exit(20);
            long r = syscall(509, id);
            if (pass == 0) _exit(r == -1 && errno == EACCES ? 0 : 21);
            _exit(r == 3 * 4096 ? 0 : 22);
        }
        int st;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            probe_fail(pass == 0 ? "another user read a 0600 object's size (status 0x%x)"
                                 : "another user could not read a 0644 object's size (status 0x%x)", st);
    }
    syscall(502, id);
    probe_pass();
}
