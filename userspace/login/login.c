#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../auth/auth.h"

#define LINE_MAX 256
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

static void copy_field(char *dst, int cap, const char *src) {
    int i = 0;
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int parse_passwd_line(char *line, const char *want_user) {
    char *save = (char *)0;
    char *name = strtok_r(line, ":", &save);
    char *passwd = strtok_r((char *)0, ":", &save);
    char *uid = strtok_r((char *)0, ":", &save);
    char *gid = strtok_r((char *)0, ":", &save);
    char *gecos = strtok_r((char *)0, ":", &save);
    char *dir = strtok_r((char *)0, ":", &save);
    char *sh = strtok_r((char *)0, ":\n\r", &save);

    (void)uid;
    (void)gid;
    (void)gecos;

    if (!name || !dir || !sh) return 0;
    if (strcmp(name, want_user) != 0) return 0;

    copy_field(username, sizeof(username), name);
    copy_field(passwd_field, sizeof(passwd_field), passwd ? passwd : "");
    copy_field(home, sizeof(home), dir);
    copy_field(shell, sizeof(shell), sh);
    return 1;
}

static int load_user(const char *name) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f && access("/disk/etc/passwd", R_OK) == 0)
        f = fopen("/disk/etc/passwd", "r");
    if (!f) return 0;

    char line[LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        if (parse_passwd_line(line, name)) {
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

static int load_shadow_password(const char *name, char *out, int out_cap) {
    FILE *f = fopen("/etc/shadow", "r");
    if (!f && access("/disk/etc/shadow", R_OK) == 0)
        f = fopen("/disk/etc/shadow", "r");
    if (!f) return 0;

    char line[LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        char *save = (char *)0;
        char *shadow_name = strtok_r(line, ":", &save);
        char *shadow_pass = strtok_r((char *)0, ":\n\r", &save);
        if (!shadow_name || !shadow_pass) continue;
        if (strcmp(shadow_name, name) == 0) {
            copy_field(out, out_cap, shadow_pass);
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

static int check_password(const char *name, const char *password) {
    char expected[FIELD_MAX];

    if (!load_user(name))
        return -1;

    if (strcmp(passwd_field, "x") == 0) {
        if (!load_shadow_password(name, expected, sizeof(expected)))
            return 0;
    } else {
        copy_field(expected, sizeof(expected), passwd_field);
    }

    return maero_password_verify(expected, password);
}

static void select_shell_path(void) {
    if (strncmp(shell, "/disk/", 6) == 0) {
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

int main(int argc, char *argv[]) {
    int forced = 0;
    const char *name = "root";

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
        forced = 1;
        name = argc > 2 ? argv[2] : "root";
    } else if (argc > 1) {
        name = argv[1];
    }

    if (!load_user(name)) {
        printf("login: unknown user %s\n", name);
        return 1;
    }

    if (!forced) {
        if (argc < 3) {
            printf("login: password required\n");
            return 1;
        }
        if (check_password(name, argv[2]) <= 0) {
            printf("login: authentication failed\n");
            return 1;
        }
    }

    select_shell_path();
    build_env();

    printf("login: %s accepted\n", username);

    char *shell_argv[] = { shell_path, (char *)0 };
    execve(shell_path, shell_argv, envp);
    printf("login: failed to exec %s\n", shell_path);
    return 1;
}
