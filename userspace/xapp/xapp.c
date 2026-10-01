/*
 * xapp — install, remove and run the curated Alpine X11 applications
 * (xapps.h) from the MaeroOS desktop.  Installed set-uid root at /disk/xapp
 * on the Alpine disk image (ports/alpine/prepare.py, ALPINE_X=1).
 *
 *   xapp list                 every curated app and whether it is installed
 *   xapp install <name>       apk add its packages in the chroot, add the
 *                             launcher entry /disk/apps/<name>/manifest
 *   xapp remove <name>        apk del it, drop the launcher entry
 *   xapp <slot> <name>        run it (the desktop launches entries this way):
 *                             start maeroX in the slot if no X server is up,
 *                             then run the app in the chroot as the caller
 *   xapp run <name>           run it from a shell (asks the desktop to
 *                             launch it when no X server is up)
 *
 * Privilege: chroot(2) and apk need root, which is why this is set-uid.  It
 * only ever acts on the names in xapps.h — no package names, paths or
 * commands come from the caller — and installs only from the Alpine root's
 * configured repositories, whose signatures apk verifies.  Apps run with the
 * caller's uid and gid: the root is entered first, then the privileges are
 * dropped for good before the app is executed.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "xapps.h"

#define APPS_DIR "/disk/apps"

static const char *const alpine_path =
    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

static int installed(const xapp_t *a) {
    char p[160];
    snprintf(p, sizeof(p), ALPINE_ROOT "%s", a->binary);
    return access(p, F_OK) == 0;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

/* Is an X server accepting on display :0 (abstract or filesystem socket)? */
static int x_up(void) {
    for (int abstract = 1; abstract >= 0; abstract--) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        const char *p = "/tmp/.X11-unix/X0";
        strcpy(a.sun_path + abstract, p);
        socklen_t len = (socklen_t)(sizeof(a.sun_family) + abstract + strlen(p));
        int ok = connect(fd, (struct sockaddr *)&a, len) == 0;
        close(fd);
        if (ok) return 1;
    }
    return 0;
}

static void notify_desktop(const char *cmd) {
    int fd = open("/tmp/wmctl", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return;
    char line[64];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    write(fd, line, (size_t)n);
    close(fd);
}

/* Run argv inside the Alpine root as root; returns the exit status. */
static int run_in_root(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        static char *const env[] = { (char *)"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
                                     (char *)"HOME=/root", (char *)"TERM=dumb",
                                     (char *)"LANG=C.UTF-8", 0 };
        if (setuid(0) != 0 || chroot(ALPINE_ROOT) != 0 || chdir("/") != 0) {
            printf("xapp: cannot enter " ALPINE_ROOT " (%d)\n", errno);
            _exit(126);
        }
        execve(argv[0], argv, env);
        printf("xapp: cannot run %s (%d)\n", argv[0], errno);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* Split a space separated list into argv after `prefix` entries. */
static int split(char *s, char **argv, int max, int at) {
    while (*s && at < max - 1) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        argv[at++] = s;
        while (*s && *s != ' ') s++;
    }
    argv[at] = 0;
    return at;
}

static int write_manifest(const xapp_t *a) {
    char dir[96], path[128], buf[256];
    mkdir(APPS_DIR, 0775);
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", a->name);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/manifest", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int n = snprintf(buf, sizeof(buf), "exec=" XAPP_HELPER "\nargs=%s\ntitle=%s\nalpine=1\n",
                     a->name, a->title);
    int ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}

static void remove_manifest(const xapp_t *a) {
    char dir[96], path[128];
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", a->name);
    snprintf(path, sizeof(path), "%s/manifest", dir);
    unlink(path);
    rmdir(dir);
}

static int do_install(const xapp_t *a) {
    char pk[160];
    char *argv[16] = { (char *)"/sbin/apk", (char *)"add", (char *)"--no-progress" };
    snprintf(pk, sizeof(pk), "%s", a->packages);
    split(pk, argv, 16, 3);
    printf("xapp: installing %s (%s)\n", a->name, a->packages);
    fflush(stdout);
    int st = run_in_root(argv);
    if (st != 0) { printf("xapp: apk add failed (%d)\n", st); return 1; }
    if (write_manifest(a) < 0) { printf("xapp: cannot write the launcher entry\n"); return 1; }
    notify_desktop("apps-changed");
    printf("xapp: %s installed\n", a->name);
    return 0;
}

static int do_remove(const xapp_t *a) {
    char pk[160];
    char *argv[8] = { (char *)"/sbin/apk", (char *)"del", (char *)"--no-progress" };
    snprintf(pk, sizeof(pk), "%s", a->packages);
    char *first = strtok(pk, " ");            /* the app itself; fonts and
                                               * themes may serve others */
    argv[3] = first;
    argv[4] = 0;
    int st = run_in_root(argv);
    if (st != 0) { printf("xapp: apk del failed (%d)\n", st); return 1; }
    remove_manifest(a);
    notify_desktop("apps-changed");
    printf("xapp: %s removed\n", a->name);
    return 0;
}

/* The caller's uid needs a name inside the Alpine root too: GLib, xterm and
 * the shell look the user up with getpwuid() and fall back badly without one
 * (no home directory, "I have no name!").  Appends "<user>:x:<uid>:<gid>"
 * to the root's /etc/passwd and /etc/group when the id is not there yet. */
static void ensure_account(const char *file, const char *line, unsigned id) {
    char buf[8192];
    int fd = open(file, O_RDONLY);
    int n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n < 0) return;
    buf[n] = 0;
    char key[24];
    snprintf(key, sizeof(key), ":x:%u:", id);
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n');
        char *k = strstr(l, key);
        if (k && (!nl || k < nl) && memchr(l, ':', (size_t)(k - l + 1)) == k) return;
        l = nl ? nl + 1 : NULL;
    }
    fd = open(file, O_WRONLY | O_APPEND);
    if (fd < 0) return;
    if (n > 0 && buf[n - 1] != '\n') write(fd, "\n", 1);
    write(fd, line, strlen(line));
    close(fd);
}

/* Enter the Alpine root, become the caller and exec the app.  Only returns
 * on failure. */
static void exec_app(const xapp_t *a, uid_t uid, gid_t gid, int wait_server) {
    if (wait_server)
        for (int i = 0; i < 200 && !x_up(); i++) sleep_ms(50);

    char user[32] = "root", home[64] = "/root", rt[48], logp[80];
    if (uid != 0) {
        struct passwd *pw = getpwuid(uid);
        snprintf(user, sizeof(user), "%s", pw && pw->pw_name ? pw->pw_name : "user");
        snprintf(home, sizeof(home), "/home/%s", user);
    }
    char path[128];
    if (uid != 0) {
        char line[160];
        snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:/bin/sh\n", user, (unsigned)uid,
                 (unsigned)gid, user, home);
        ensure_account(ALPINE_ROOT "/etc/passwd", line, (unsigned)uid);
        snprintf(line, sizeof(line), "%s:x:%u:\n", user, (unsigned)gid);
        ensure_account(ALPINE_ROOT "/etc/group", line, (unsigned)gid);
    }
    snprintf(path, sizeof(path), ALPINE_ROOT "%s", home);
    mkdir(ALPINE_ROOT "/home", 0755);
    if (mkdir(path, 0700) == 0 || errno == EEXIST) chown(path, uid, gid);
    /* XDG directories apps expect to exist (galculator does not create
     * ~/.config itself and then cannot keep its settings). */
    static const char *const xdg[] = { "/.config", "/.cache", "/.local", "/.local/share" };
    for (unsigned i = 0; i < sizeof(xdg) / sizeof(xdg[0]); i++) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s%s", path, xdg[i]);
        if (mkdir(sub, 0700) == 0) chown(sub, uid, gid);
    }
    snprintf(rt, sizeof(rt), "/tmp/runtime-%u", (unsigned)uid);
    snprintf(path, sizeof(path), ALPINE_ROOT "%s", rt);
    if (mkdir(path, 0700) == 0 || errno == EEXIST) chown(path, uid, gid);

    /* The app's own output goes to /tmp/xapp-<name>.log in the root. */
    snprintf(logp, sizeof(logp), ALPINE_ROOT "/tmp/xapp-%s.log", a->name);
    int lfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) {
        fchown(lfd, uid, gid);
        dup2(lfd, 1);
        dup2(lfd, 2);
        if (lfd > 2) close(lfd);
    }
    int nfd = open("/dev/null", O_RDONLY);
    if (nfd >= 0) { dup2(nfd, 0); if (nfd > 0) close(nfd); }

    if (chroot(ALPINE_ROOT) != 0 || chdir("/") != 0) {
        printf("xapp: cannot enter " ALPINE_ROOT " (%d)\n", errno);
        return;
    }
    chdir(home);
    gid_t none[1] = { gid };
    if (setgroups(1, none) != 0 || setgid(gid) != 0 || setuid(uid) != 0 ||
        getuid() != (int)uid || geteuid() != (int)uid || (uid != 0 && setuid(0) == 0)) {
        printf("xapp: cannot drop privileges\n");
        return;
    }

    char cmd[160];
    char *argv[16];
    snprintf(cmd, sizeof(cmd), "%s", a->command);
    split(cmd, argv, 16, 0);
    static char e_home[80], e_user[48], e_rt[64];
    snprintf(e_home, sizeof(e_home), "HOME=%s", home);
    snprintf(e_user, sizeof(e_user), "USER=%s", user);
    snprintf(e_rt, sizeof(e_rt), "XDG_RUNTIME_DIR=%s", rt);
    char *env[] = {
        (char *)alpine_path, e_home, e_user, e_rt,
        (char *)"DISPLAY=:0", (char *)"LANG=C.UTF-8", (char *)"TERM=xterm",
        (char *)"SHELL=/bin/sh",
        /* No session bus or accessibility bus on MaeroOS: say so up front
         * instead of letting GLib wait for one. */
        (char *)"NO_AT_BRIDGE=1", (char *)"GTK_A11Y=none",
        (char *)"DBUS_SESSION_BUS_ADDRESS=disabled:",
        (char *)"GDK_BACKEND=x11",
        0,
    };
    char exe[96];
    if (argv[0][0] == '/') snprintf(exe, sizeof(exe), "%s", argv[0]);
    else snprintf(exe, sizeof(exe), "/usr/bin/%s", argv[0]);
    printf("xapp: %s as uid %u\n", exe, (unsigned)uid);
    fflush(stdout);
    execve(exe, argv, env);
    printf("xapp: exec %s failed (%d)\n", exe, errno);
}

static const char *maerox_path(void) {
    return access("/disk/maerox", X_OK) == 0 ? "/disk/maerox" : "/maerox";
}

static int do_run(const xapp_t *a, int slot) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (!installed(a)) { printf("xapp: %s is not installed\n", a->name); return 1; }
    if (x_up()) {
        exec_app(a, uid, gid, 0);
        return 1;
    }
    if (slot <= 0) {
        /* From a shell: the desktop owns the window slots. */
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "launch %s", a->name);
        notify_desktop(cmd);
        printf("xapp: asked the desktop to launch %s\n", a->name);
        return 0;
    }
    /* No server yet: this process becomes maeroX in our window slot, the
     * child becomes the app once the server listens. */
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        exec_app(a, uid, gid, 1);
        _exit(127);
    }
    gid_t none[1] = { gid };
    if (setgroups(1, none) != 0 || setgid(gid) != 0 || setuid(uid) != 0 ||
        (uid != 0 && setuid(0) == 0)) {
        printf("xapp: cannot drop privileges\n");
        return 1;
    }
    char s[8];
    snprintf(s, sizeof(s), "%d", slot);
    char *argv[] = { (char *)maerox_path(), s, 0 };
    char *env[] = { (char *)"PATH=/disk:/bin:/", (char *)"HOME=/tmp", 0 };
    execve(argv[0], argv, env);
    printf("xapp: cannot start maeroX (%d)\n", errno);
    return 1;
}

static void usage(void) {
    printf("usage: xapp list | install <name> | remove <name> | run <name> | <slot> <name>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (access(ALPINE_ROOT "/etc/alpine-release", F_OK) != 0) {
        printf("xapp: no Alpine root at " ALPINE_ROOT " (boot with disk-alpinex.img)\n");
        return 1;
    }
    if (!strcmp(argv[1], "list")) {
        for (int i = 0; i < N_XAPPS; i++)
            printf("%-11s %-10s %s\n", xapps[i].name,
                   installed(&xapps[i]) ? "installed" : "-", xapps[i].about);
        return 0;
    }
    if (argc < 3) { usage(); return 2; }
    const xapp_t *a = xapp_find(argv[2]);
    if (!a) { printf("xapp: %s is not one of the curated apps (xapp list)\n", argv[2]); return 2; }
    if (!strcmp(argv[1], "install")) return do_install(a);
    if (!strcmp(argv[1], "remove")) return do_remove(a);
    if (!strcmp(argv[1], "run")) return do_run(a, 0);
    if (argv[1][0] >= '0' && argv[1][0] <= '9') return do_run(a, atoi(argv[1]));
    usage();
    return 2;
}
