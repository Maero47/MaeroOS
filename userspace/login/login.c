#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/grp.h"
#include "../auth/auth.h"

#define FIELD_MAX 192

static char username[FIELD_MAX];
static char passwd_field[FIELD_MAX];
static char home[FIELD_MAX];
static char shell[FIELD_MAX];
static char shell_path[FIELD_MAX];
static char env_path[] = "PATH=/disk:/disk/bin:/:/bin";
static char env_home[FIELD_MAX + 6];
static char env_user[FIELD_MAX + 6];
static char env_logname[FIELD_MAX + 9];
static char env_shell[FIELD_MAX + 7];
static char env_term[] = "TERM=linux";
static char env_tty[] = "TTY=tty0";
/* Dynamic-loader search path: covers musl (/lib, /disk/lib) and the glibc
 * library stacks shipped on disk (GTK/X11/Firefox in /disk/firefox).  glibc's
 * ld.so reads this from the environment, so it must be inherited from login. */
static char env_ldpath[] = "LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox";
/* Default X display so GUI clients (Firefox/GTK) find maeroX without -display. */
static char env_display[] = "DISPLAY=:0";
/* gdk-pixbuf image-loader cache: Firefox's chrome icons are PNG decoded through
 * gdk-pixbuf, which finds its loader modules via this cache file.  Without it,
 * every icon load fails (GDK_IS_PIXBUF assertion) and the chrome never finishes
 * → the browser window is never shown.  PNG/JPEG are built into the shipped
 * libgdk_pixbuf; the cache covers the external formats (gif/bmp/ico/…). */
static char env_pixbuf[] =
    "GDK_PIXBUF_MODULE_FILE=/disk/firefox/pixbuf-loaders/loaders.cache";
static char *envp[] = {
    env_path,
    env_home,
    env_user,
    env_logname,
    env_shell,
    env_term,
    env_tty,
    env_ldpath,
    env_display,
    env_pixbuf,
    (char *)0
};

static int user_uid = -1;
static int user_gid = -1;

static void copy_field(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* name:passwd:uid:gid:gecos:home:shell -- exactly seven fields, any of
 * which (gecos in particular) may be empty. */
static int parse_passwd_line(char *line, const char *want_user) {
    char *f[MAERO_PASSWD_FIELDS];
    int uid, gid;

    if (maero_split_fields(line, f, MAERO_PASSWD_FIELDS) != MAERO_PASSWD_FIELDS)
        return 0;
    if (strcmp(f[0], want_user) != 0) return 0;
    if (!maero_parse_id(f[2], &uid) || !maero_parse_id(f[3], &gid)) return 0;
    if (!f[5][0] || !f[6][0]) return 0;

    copy_field(username, sizeof(username), f[0]);
    copy_field(passwd_field, sizeof(passwd_field), f[1]);
    copy_field(home, sizeof(home), f[5]);
    copy_field(shell, sizeof(shell), f[6]);
    user_uid = uid;
    user_gid = gid;
    return 1;
}

static int load_user(const char *name) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f && access("/disk/etc/passwd", R_OK) == 0)
        f = fopen("/disk/etc/passwd", "r");
    if (!f) return 0;

    char line[MAERO_LINE_MAX];
    while (maero_read_line(f, line, sizeof(line))) {
        if (parse_passwd_line(line, name)) {
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

static int check_password(const char *name, const char *password) {
    char expected[MAERO_HASH_MAX];
    int ok;

    if (!load_user(name))
        return -1;

    if (strcmp(passwd_field, "x") == 0) {
        if (!maero_shadow_hash(name, expected, sizeof(expected)))
            return 0;
    } else {
        copy_field(expected, sizeof(expected), passwd_field);
    }

    ok = maero_password_verify(expected, password);
    maero_wipe(expected, sizeof(expected));
    return ok;
}

/* Become the target user: group first (it needs root), then uid.  Every
 * step is checked and a failure aborts the login -- continuing would hand
 * out a shell with the caller's (usually root's) privileges. */
static int drop_privileges(void) {
    if (user_uid < 0 || user_gid < 0) return 0;
    if (setgid(user_gid) != 0) return 0;
    if (initgroups(username, user_gid) != 0) return 0;
    if (setuid(user_uid) != 0) return 0;
    if (getuid() != user_uid || geteuid() != user_uid) return 0;
    if (getgid() != user_gid || getegid() != user_gid) return 0;
    /* A non-root user must not be able to get root back. */
    if (user_uid != 0 && setuid(0) == 0) return 0;
    return 1;
}

static void select_shell_path(void) {
    if (strncmp(shell, "/disk/", 6) == 0) {
        /* Initrd-only boot: no /disk, so fall back to the initrd copy
         * (/disk/shell -> /shell) rather than fail to start a shell. */
        if (access(shell, X_OK) != 0 && access(shell + 5, X_OK) == 0)
            copy_field(shell_path, sizeof(shell_path), shell + 5);
        else
            copy_field(shell_path, sizeof(shell_path), shell);
        return;
    }

    if (strcmp(shell, "/shell") == 0 && access("/disk/shell", X_OK) == 0) {
        copy_field(shell_path, sizeof(shell_path), "/disk/shell");
        return;
    }

    copy_field(shell_path, sizeof(shell_path), shell);
}

static void build_env(void) {
    snprintf(env_home, sizeof(env_home), "HOME=%s", home[0] ? home : "/");
    snprintf(env_user, sizeof(env_user), "USER=%s", username);
    snprintf(env_logname, sizeof(env_logname), "LOGNAME=%s", username);
    snprintf(env_shell, sizeof(env_shell), "SHELL=%s", shell_path);
}

/* A rejected login waits before it exits, as shadow's FAIL_DELAY does: it
 * slows password guessing on the console, and it keeps a getty session from
 * ending in under 3 seconds, which init would count as a rapid failure and,
 * after 8 in a row, park the console. */
static void fail_delay(void) {
    for (int i = 0; i < 6; i++)
        usleep(500000);
}

int main(int argc, char *argv[]) {
    int forced = 0;
    const char *name = "root";
    char password[128];

    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        if (argc < 4) {
            printf("login: usage: login --check USER PASSWORD\n");
            return 1;
        }

        int ok = check_password(argv[2], argv[3]);
        if (ok > 0) {
            printf("login: %s password ok\n", username);
            return 0;
        }
        if (ok < 0)
            printf("login: unknown user %s\n", argv[2]);
        else
            printf("login: authentication failed\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        /* Pre-authenticated login is for getty/init only. */
        if (getuid() != 0) {
            printf("login: -f requires root\n");
            return 1;
        }
        forced = 1;
        name = argc > 2 ? argv[2] : "root";
    } else if (argc > 1) {
        name = argv[1];
    }

    if (!load_user(name)) {
        if (!forced) {
            /* Ask for the password anyway, so the console does not tell
             * which account names exist. */
            if (argc < 3 && maero_read_password("Password: ", password, sizeof(password)))
                maero_wipe(password, sizeof(password));
            fail_delay();
            printf("login: authentication failed\n");
            return 1;
        }
        printf("login: unknown user %s\n", name);
        return 1;
    }

    if (!forced) {
        int ok;

        /* Prefer the prompt: a password in argv is visible to every
         * process via /proc/<pid>/cmdline.  The argv form is kept for
         * scripts. */
        if (argc >= 3) {
            copy_field(password, sizeof(password), argv[2]);
        } else if (!maero_read_password("Password: ", password, sizeof(password))) {
            fail_delay();
            printf("login: password required\n");
            return 1;
        }
        ok = check_password(name, password);
        maero_wipe(password, sizeof(password));
        if (ok <= 0) {
            fail_delay();
            printf("login: authentication failed\n");
            return 1;
        }
    }

    select_shell_path();
    build_env();

    if (!drop_privileges()) {
        printf("login: cannot drop privileges to %s\n", username);
        return 1;
    }
    if (chdir(home) != 0) {
        printf("login: no home directory %s, using /\n", home);
        chdir("/");
    }

    printf("login: %s accepted\n", username);

    char *shell_argv[] = { shell_path, (char *)0 };
    execve(shell_path, shell_argv, envp);
    printf("login: failed to exec %s\n", shell_path);
    return 1;
}
