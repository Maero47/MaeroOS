/*
 * passwd — change a password in /etc/shadow.  Installed set-uid root.
 *
 *   passwd USER            prompt for the old and new password
 *   passwd USER OLD NEW    non-interactive (the passwords show in argv)
 *
 * The new file is built in memory, written to a root-owned 0600 temp file
 * in the same directory, flushed and checked, and only then renamed over
 * /etc/shadow.  Any failure leaves the old file in place.
 */
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/fcntl.h"
#include "../auth/auth.h"

#define SHADOW_MAX 16384

static char in_buf[SHADOW_MAX];
static char out_buf[SHADOW_MAX + MAERO_HASH_MAX + 64];
static char old_pw[128], new_pw[128], again_pw[128];

static void wipe_all(void) {
    maero_wipe(in_buf, sizeof(in_buf));
    maero_wipe(out_buf, sizeof(out_buf));
    maero_wipe(old_pw, sizeof(old_pw));
    maero_wipe(new_pw, sizeof(new_pw));
    maero_wipe(again_pw, sizeof(again_pw));
}

static int valid_password(const char *password) {
    if (!password || !password[0])
        return 0;
    for (int i = 0; password[i]; i++) {
        if (password[i] == ':' || password[i] == '\n' || password[i] == '\r')
            return 0;
    }
    return 1;
}

static int copy_arg(char *dst, int cap, const char *src) {
    if ((int)strlen(src) >= cap)
        return 0;
    strcpy(dst, src);
    return 1;
}

static int read_all(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    int len = 0;

    if (fd < 0)
        return -1;
    for (;;) {
        int n = read(fd, buf + len, cap - 1 - len);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        len += n;
        if (len == cap - 1) { close(fd); return -1; }   /* too large */
    }
    close(fd);
    buf[len] = '\0';
    return len;
}

static int write_all(int fd, const char *buf, int len) {
    while (len > 0) {
        int n = write(fd, buf, len);
        if (n <= 0)
            return 0;
        buf += n;
        len -= n;
    }
    return 1;
}

/* Force root:root 0600 on an open shadow file. */
static int lock_down(int fd) {
    return fchown(fd, 0, 0) == 0 && fchmod(fd, 0600) == 0;
}

/* Rebuild the shadow text with USER's hash replaced.  Returns 1 on success,
 * 0 for unknown user, -1 for a wrong password, -2 on an internal error. */
static int rebuild(const char *user, int *out_len) {
    char salt[33];
    char hashed[MAERO_HASH_MAX];
    char stored[MAERO_HASH_MAX];
    int user_len = strlen(user);
    int found = 0, pos = 0;
    char *line = in_buf;

    while (*line) {
        char *eol = strchr(line, '\n');
        int line_len = eol ? (int)(eol - line) + 1 : (int)strlen(line);
        char *colon = line;

        while (colon < line + line_len && *colon != ':')
            colon++;
        if (colon == line + line_len)
            colon = 0;

        if (!found && colon && colon - line == user_len &&
            strncmp(line, user, user_len) == 0) {
            char *pass = colon + 1;
            char *rest = pass;
            int pass_len;

            found = 1;
            while (rest < line + line_len && *rest != ':' && *rest != '\n' && *rest != '\r')
                rest++;
            pass_len = rest - pass;
            if (pass_len >= (int)sizeof(stored))
                return -1;
            memcpy(stored, pass, pass_len);
            stored[pass_len] = '\0';
            if (!maero_password_verify(stored, old_pw))
                return -1;
            if (!maero_password_make_salt(user, salt, sizeof(salt)) ||
                !maero_password_hash(salt, new_pw, hashed, sizeof(hashed)))
                return -2;
            pos += snprintf(out_buf + pos, sizeof(out_buf) - pos, "%s:%s", user, hashed);
            line_len -= rest - line;
            line = rest;
            maero_wipe(hashed, sizeof(hashed));
            maero_wipe(stored, sizeof(stored));
        }
        if (pos + line_len >= (int)sizeof(out_buf))
            return -2;
        memcpy(out_buf + pos, line, line_len);
        pos += line_len;
        line += line_len;
    }
    *out_len = pos;
    return found;
}

int main(int argc, char *argv[]) {
    const char *user;
    const char *dir;
    char shadow[32], tmp[40];
    int len, fd, r;

    if (argc != 2 && argc != 4) {
        printf("passwd: usage: passwd USER [OLD NEW]\n");
        return 1;
    }
    user = argv[1];

    /* Run as root through and through, so nothing below (in particular the
     * temp file, which the kernel stamps with the real uid) is ever owned
     * by the caller. */
    if (setgid(0) != 0 || setuid(0) != 0 || getuid() != 0 || getgid() != 0) {
        printf("passwd: cannot become root (not installed set-uid root?)\n");
        return 1;
    }

    if (argc == 4) {
        if (!copy_arg(old_pw, sizeof(old_pw), argv[2]) ||
            !copy_arg(new_pw, sizeof(new_pw), argv[3])) {
            printf("passwd: password too long\n");
            return 1;
        }
    } else {
        maero_read_password("Old password: ", old_pw, sizeof(old_pw));
        maero_read_password("New password: ", new_pw, sizeof(new_pw));
        maero_read_password("Retype new password: ", again_pw, sizeof(again_pw));
        if (strcmp(new_pw, again_pw) != 0) {
            wipe_all();
            printf("passwd: passwords do not match\n");
            return 1;
        }
    }

    if (!valid_password(new_pw)) {
        wipe_all();
        printf("passwd: invalid new password\n");
        return 1;
    }

    dir = access("/etc/shadow", R_OK) == 0 ? "/etc" : "/disk/etc";
    snprintf(shadow, sizeof(shadow), "%s/shadow", dir);
    snprintf(tmp, sizeof(tmp), "%s/shadow.tmp", dir);

    if (read_all(shadow, in_buf, sizeof(in_buf)) < 0) {
        wipe_all();
        printf("passwd: cannot read /etc/shadow\n");
        return 1;
    }

    r = rebuild(user, &len);
    if (r <= 0) {
        wipe_all();
        if (r == 0)
            printf("passwd: unknown user %s\n", user);
        else if (r == -1)
            printf("passwd: authentication failed\n");
        else
            printf("passwd: failed to hash password\n");
        return 1;
    }

    /* /etc is root-only (tools/diskperms.txt), so a stale temp file can only
     * be ours; remove it so O_EXCL creates a fresh inode. */
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC);
    if (fd < 0) {
        wipe_all();
        printf("passwd: cannot write %s\n", tmp);
        return 1;
    }
    if (!lock_down(fd) || !write_all(fd, out_buf, len) || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        wipe_all();
        printf("passwd: failed to write %s\n", tmp);
        return 1;
    }
    wipe_all();
    if (close(fd) != 0) {
        unlink(tmp);
        printf("passwd: failed to write %s\n", tmp);
        return 1;
    }

    if (rename(tmp, shadow) != 0) {
        unlink(tmp);
        printf("passwd: failed to update /etc/shadow\n");
        return 1;
    }

    /* The kernel's rename() does not yet carry the source's owner and mode
     * over; re-apply them so the new shadow is never left world-readable. */
    fd = open(shadow, O_RDONLY);
    if (fd < 0 || !lock_down(fd)) {
        if (fd >= 0) close(fd);
        printf("passwd: WARNING: could not restrict %s to 0600\n", shadow);
        return 1;
    }
    close(fd);

    printf("passwd: password updated for %s\n", user);
    return 0;
}
