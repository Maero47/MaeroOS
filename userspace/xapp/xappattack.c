/*
 * xappattack — regression probe for the set-uid helper /disk/xapp
 * (smoke-alpinex).  Run as root with maeroX already up:
 *
 *   xappattack <installed app>
 *
 * As uid/gid 1000 it plants the symlinks a local user could plant in the
 * places xapp works in, pointing at root-owned canaries, then runs
 * `/disk/xapp run <app>` (set-uid root) and kills the app it started:
 *   /disk/alpine/tmp/xapp-<app>.log -> /disk/xapp-canary-log   (truncate + chown)
 *   /disk/alpine/tmp/runtime-1000   -> /disk/xapp-canary-rt    (chown)
 *   ~/.local                        -> /disk/xapp-canary-dir   (mkdir + chown)
 * Each canary must come out untouched and root's.  Prints
 * "[xappattack] PASS" or "[xappattack] FAIL <what>".
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UID 1000
#define CANARY "CANARY\n"

static int fails;
static void fail(const char *what) { printf("[xappattack] FAIL %s\n", what); fails++; }

static void write_canary(const char *p) {
    unlink(p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, CANARY, 7); close(fd); }
}

static void check_file(const char *p) {
    char b[16] = { 0 };
    struct stat st;
    int fd = open(p, O_RDONLY);
    int n = fd >= 0 ? (int)read(fd, b, sizeof(b) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n != 7 || strcmp(b, CANARY) != 0) { fail(p); return; }
    if (stat(p, &st) != 0 || st.st_uid != 0) fail(p);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: xappattack <installed app>\n"); return 2; }
    char log[128], rt[64] = "/disk/alpine/tmp/runtime-1000", local[96];
    snprintf(log, sizeof(log), "/disk/alpine/tmp/xapp-%s.log", argv[1]);
    snprintf(local, sizeof(local), "/disk/alpine/home/user/.local");

    write_canary("/disk/xapp-canary-log");
    write_canary("/disk/xapp-canary-rt");
    rmdir("/disk/xapp-canary-dir/share");
    mkdir("/disk/xapp-canary-dir", 0755);
    chown("/disk/xapp-canary-dir", 0, 0);
    /* The home xapp would have made on a first run, as the user's. */
    mkdir("/disk/alpine/home", 0755);
    mkdir("/disk/alpine/home/user", 0700);
    chown("/disk/alpine/home/user", UID, UID);

    pid_t pid = fork();
    if (pid == 0) {
        if (setgid(UID) != 0 || setuid(UID) != 0) _exit(126);
        /* Clear the way (as the user, as an attacker would). */
        unlink(log);
        rmdir(rt); unlink(rt);
        rmdir("/disk/alpine/home/user/.local/share");
        rmdir(local); unlink(local);
        if (symlink("/disk/xapp-canary-log", log) != 0) printf("[xappattack] cannot plant %s\n", log);
        if (symlink("/disk/xapp-canary-rt", rt) != 0) printf("[xappattack] cannot plant %s\n", rt);
        if (symlink("/disk/xapp-canary-dir", local) != 0) printf("[xappattack] cannot plant %s\n", local);
        char *av[] = { (char *)"/disk/xapp", (char *)"run", argv[1], 0 };
        char *env[] = { (char *)"PATH=/bin", 0 };
        execve(av[0], av, env);
        _exit(127);
    }
    struct timespec ts = { 4, 0 };
    nanosleep(&ts, 0);
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);

    check_file("/disk/xapp-canary-log");
    check_file("/disk/xapp-canary-rt");
    struct stat st;
    if (lstat("/disk/xapp-canary-dir/share", &st) == 0) fail("/disk/xapp-canary-dir/share created");
    if (stat("/disk/xapp-canary-dir", &st) != 0 || st.st_uid != 0) fail("/disk/xapp-canary-dir chowned");

    unlink(log); unlink(rt); unlink(local);
    unlink("/disk/xapp-canary-log"); unlink("/disk/xapp-canary-rt");
    rmdir("/disk/xapp-canary-dir");
    if (!fails) printf("[xappattack] PASS\n");
    return fails ? 1 : 0;
}
