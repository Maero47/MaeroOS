/*
 * doas — run a command as root after verifying the caller's password.
 * Installed set-uid root (mode 04755).  Like OpenBSD doas / sudo.
 *
 *   doas <command> [args...]
 *
 * The caller (real uid) must be listed in /etc/doas.conf as "permit <user>".
 * We prompt for THEIR password, verify it against /etc/shadow (readable
 * because exec already gave us euid 0 from the set-uid bit), then setuid(0)
 * and exec the command with a root environment.
 */
#include "../include/stdio.h"
#include "../include/stdlib.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../include/termios.h"
#include "../auth/auth.h"

#define LINE 256

static char username[64];

/* Resolve our real uid to a username via /etc/passwd. */
static int lookup_user(int uid) {
    FILE *f = fopen("/etc/passwd", "r");
    char line[LINE];

    if (!f && access("/disk/etc/passwd", 0) == 0)
        f = fopen("/disk/etc/passwd", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *save = 0;
        char *name = strtok_r(line, ":", &save);
        char *x    = strtok_r(0, ":", &save);   /* passwd field */
        char *uids = strtok_r(0, ":", &save);
        (void)x;
        if (name && uids && atoi(uids) == uid) {
            strncpy(username, name, sizeof(username) - 1);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* /etc/doas.conf: lines "permit <user>".  Root is always permitted. */
static int permitted(const char *user) {
    FILE *f;
    char line[LINE];

    if (strcmp(user, "root") == 0) return 1;
    f = fopen("/etc/doas.conf", "r");
    if (!f && access("/disk/etc/doas.conf", 0) == 0)
        f = fopen("/disk/etc/doas.conf", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *save = 0;
        char *verb = strtok_r(line, " \t\r\n", &save);
        char *who  = strtok_r(0, " \t\r\n", &save);
        if (verb && who && strcmp(verb, "permit") == 0 &&
            strcmp(who, user) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int shadow_hash(const char *user, char *out, int cap) {
    FILE *f = fopen("/etc/shadow", "r");
    char line[LINE];

    if (!f && access("/disk/etc/shadow", 0) == 0)
        f = fopen("/disk/etc/shadow", "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *save = 0;
        char *name = strtok_r(line, ":", &save);
        char *hash = strtok_r(0, ":\r\n", &save);
        if (name && hash && strcmp(name, user) == 0) {
            strncpy(out, hash, cap - 1);
            out[cap - 1] = 0;
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static void read_password(char *buf, int cap) {
    struct termios t, saved;
    int have_tty = (tcgetattr(0, &t) == 0);

    printf("doas: password for %s: ", username);
    fflush(stdout);
    if (have_tty) {
        saved = t;
        t.c_lflag &= ~ECHO;
        tcsetattr(0, TCSETS, &t);
    }
    int n = 0;
    char c;
    while (n < cap - 1 && read(0, &c, 1) == 1) {
        if (c == '\n' || c == '\r') break;
        if (c == 127 || c == 8) { if (n) n--; continue; }
        buf[n++] = c;
    }
    buf[n] = 0;
    if (have_tty) tcsetattr(0, TCSETS, &saved);
    printf("\n");
}

int main(int argc, char *argv[]) {
    char hash[MAERO_HASH_MAX], pw[128];

    if (argc < 2) {
        printf("usage: doas <command> [args...]\n");
        return 1;
    }
    if (!lookup_user(getuid())) {
        printf("doas: cannot identify calling user\n");
        return 1;
    }
    if (!permitted(username)) {
        printf("doas: %s is not permitted to run doas\n", username);
        return 1;
    }
    /* Root (or an already-root caller) skips the password. */
    if (getuid() != 0) {
        if (!shadow_hash(username, hash, sizeof(hash))) {
            printf("doas: no password entry for %s\n", username);
            return 1;
        }
        read_password(pw, sizeof(pw));
        if (!maero_password_verify(hash, pw)) {
            printf("doas: authentication failed\n");
            return 1;
        }
    }

    if (setuid(0) != 0) {
        printf("doas: setuid(0) failed (not installed set-uid root?)\n");
        return 1;
    }

    char *envp[] = {
        "PATH=/disk:/disk/bin:/:/bin",
        "HOME=/home/root", "USER=root", "LOGNAME=root", "TERM=linux", 0
    };
    execve(argv[1], &argv[1], envp);
    /* Fall back to PATH search via the shell if the literal path failed. */
    {
        char cmd[512];
        int off = 0;
        for (int i = 1; argv[i] && off < (int)sizeof(cmd) - 2; i++) {
            int l = strlen(argv[i]);
            if (off + l + 2 >= (int)sizeof(cmd)) break;
            if (i > 1) cmd[off++] = ' ';
            memcpy(cmd + off, argv[i], l);
            off += l;
        }
        cmd[off] = 0;
        char *sh = access("/disk/shell", X_OK) == 0 ? "/disk/shell" : "/shell";
        char *sargv[] = { sh, "-c", cmd, 0 };
        execve(sh, sargv, envp);
    }
    printf("doas: failed to exec %s\n", argv[1]);
    return 127;
}
