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
#include "../include/grp.h"
#include "../auth/auth.h"

#define LINE 256

/* Commands are looked up here only -- never in the caller's cwd or PATH. */
static const char *const root_path[] = { "/disk", "/disk/bin", "", "/bin", 0 };

static char username[64];

/* Resolve our real uid to a username via /etc/passwd. */
static int lookup_user(int uid) {
    FILE *f = fopen("/etc/passwd", "r");
    char line[MAERO_LINE_MAX];
    char *fld[MAERO_PASSWD_FIELDS];

    if (!f && access("/disk/etc/passwd", 0) == 0)
        f = fopen("/disk/etc/passwd", "r");
    if (!f) return 0;
    while (maero_read_line(f, line, sizeof(line))) {
        int id;
        if (maero_split_fields(line, fld, MAERO_PASSWD_FIELDS) != MAERO_PASSWD_FIELDS)
            continue;
        if (!maero_parse_id(fld[2], &id) || id != uid)
            continue;
        if (!fld[0][0] || strlen(fld[0]) >= sizeof(username))
            continue;
        strcpy(username, fld[0]);
        fclose(f);
        return 1;
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
    while (maero_read_line(f, line, sizeof(line))) {
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

/* Become root completely: gid first, then uid, each checked. */
static int become_root(void) {
    if (setgid(0) != 0) return 0;
    if (initgroups("root", 0) != 0) return 0;
    if (setuid(0) != 0) return 0;
    return getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0;
}

int main(int argc, char *argv[]) {
    char hash[MAERO_HASH_MAX], pw[128];
    char path[256];

    if (argc < 2 || !argv[1][0]) {
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
        int ok;
        char prompt[96];

        if (!maero_shadow_hash(username, hash, sizeof(hash))) {
            printf("doas: no password entry for %s\n", username);
            return 1;
        }
        snprintf(prompt, sizeof(prompt), "doas: password for %s: ", username);
        maero_read_password(prompt, pw, sizeof(pw));
        ok = maero_password_verify(hash, pw);
        maero_wipe(pw, sizeof(pw));
        maero_wipe(hash, sizeof(hash));
        if (!ok) {
            printf("doas: authentication failed\n");
            return 1;
        }
    }

    if (!become_root()) {
        printf("doas: cannot become root (not installed set-uid root?)\n");
        return 1;
    }

    char *envp[] = {
        "PATH=/disk:/disk/bin:/:/bin",
        "HOME=/home/root", "USER=root", "LOGNAME=root", "TERM=linux", 0
    };
    /* argv[1..] goes to the command unchanged -- no shell re-parsing. */
    if (strchr(argv[1], '/')) {
        execve(argv[1], &argv[1], envp);
    } else {
        for (int i = 0; root_path[i]; i++) {
            if (snprintf(path, sizeof(path), "%s/%s", root_path[i], argv[1]) >=
                (int)sizeof(path))
                continue;
            if (access(path, X_OK) != 0)
                continue;
            execve(path, &argv[1], envp);
        }
    }
    printf("doas: failed to exec %s\n", argv[1]);
    return 127;
}
