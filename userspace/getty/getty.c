#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"

#define NAME_MAX_LEN 32

static char *disk_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    "HOME=/",
    "TERM=linux",
    "TTY=tty0",
    (char *)0
};

static char *initrd_envp[] = {
    "PATH=/:/bin",
    "HOME=/",
    "TERM=linux",
    "TTY=tty0",
    (char *)0
};

static char hostname[64] = "maeros";

static void load_hostname(void) {
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) return;
    int n = read(fd, hostname, sizeof(hostname) - 1);
    close(fd);
    if (n <= 0) {
        strcpy(hostname, "maeros");
        return;
    }
    hostname[n] = '\0';
    for (int i = 0; i < n; i++) {
        if (hostname[i] == '\n' || hostname[i] == '\r' || hostname[i] == ' ') {
            hostname[i] = '\0';
            break;
        }
    }
    if (!hostname[0]) strcpy(hostname, "maeros");
}

/* One line from the console (canonical mode, echo on).  Returns the length,
 * or -1 on EOF/error.  Overlong input is read to the newline and rejected. */
static int read_name(char *buf, int cap) {
    int n = 0, overlong = 0;
    char c;
    while (1) {
        if (read(0, &c, 1) != 1) return -1;
        if (c == '\n' || c == '\r') break;
        if (n < cap - 1) buf[n++] = c;
        else overlong = 1;
    }
    buf[n] = '\0';
    return overlong ? cap : n;
}

/* A user name goes to login as argv[1], and getty runs as root: a name
 * starting with '-' would be taken as an option (`-f root` skips the
 * password), so only plain account names get through. */
static int valid_name(const char *s) {
    if (!s[0] || s[0] == '-') return 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    const char *tty = argc > 1 ? argv[1] : "tty0";
    int disk_login = access("/disk/login", X_OK) == 0;
    char *login_path = disk_login ? "/disk/login" : "/login";
    char **envp = disk_login ? disk_envp : initrd_envp;
    char name[NAME_MAX_LEN + 1];

    /* A session of our own, so the console becomes this login's
     * controlling terminal (and init's other children never hold it). */
    setsid();
    load_hostname();

    printf("\nMaeroOS getty on %s\n", tty);
    while (1) {
        printf("\n%s login: ", hostname);
        fflush(stdout);
        int n = read_name(name, sizeof(name));
        if (n < 0) {
            /* No console to read from: exit and let init respawn us. */
            usleep(500000);
            return 1;
        }
        if (n == 0) continue;
        if (n > NAME_MAX_LEN || !valid_name(name)) {
            printf("getty: invalid user name\n");
            continue;
        }
        break;
    }

    char *login_argv[] = { login_path, name, (char *)0 };
    execve(login_path, login_argv, envp);
    printf("getty: failed to exec %s\n", login_path);
    usleep(500000);
    return 1;
}
