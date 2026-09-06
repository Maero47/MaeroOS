#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../auth/auth.h"

#define LINE_MAX 256

static int user_matches(const char *line, const char *user, const char **old_pass, const char **rest) {
    int user_len = strlen(user);
    const char *first_colon = line;
    while (*first_colon && *first_colon != ':')
        first_colon++;

    if (*first_colon != ':')
        return 0;
    if (first_colon - line != user_len)
        return 0;
    if (strncmp(line, user, user_len) != 0)
        return 0;

    const char *pass = first_colon + 1;
    const char *second_colon = pass;
    while (*second_colon && *second_colon != ':' && *second_colon != '\n' && *second_colon != '\r')
        second_colon++;

    *old_pass = pass;
    *rest = second_colon;
    return 1;
}

static int password_equal(const char *start, const char *end, const char *password) {
    char stored[MAERO_HASH_MAX];
    int len = end - start;
    if (len >= (int)sizeof(stored))
        len = sizeof(stored) - 1;
    for (int i = 0; i < len; i++)
        stored[i] = start[i];
    stored[len] = '\0';
    return maero_password_verify(stored, password);
}

static int write_updated_entry(FILE *out, const char *user, const char *new_password, const char *rest) {
    char salt[33];
    char hashed[MAERO_HASH_MAX];

    if (!maero_password_make_salt(user, salt, sizeof(salt)))
        return 0;
    if (!maero_password_hash(salt, new_password, hashed, sizeof(hashed)))
        return 0;

    fprintf(out, "%s:%s%s", user, hashed, rest);
    return 1;
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

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("passwd: usage: passwd USER OLD NEW\n");
        return 1;
    }

    const char *user = argv[1];
    const char *old_password = argv[2];
    const char *new_password = argv[3];

    if (!valid_password(new_password)) {
        printf("passwd: invalid new password\n");
        return 1;
    }

    FILE *in = fopen("/etc/shadow", "r");
    if (!in && access("/disk/etc/shadow", R_OK) == 0)
        in = fopen("/disk/etc/shadow", "r");
    if (!in) {
        printf("passwd: cannot open /etc/shadow\n");
        return 1;
    }

    FILE *out = fopen("/etc/shadow.tmp", "w");
    if (!out && access("/disk/etc", W_OK) == 0)
        out = fopen("/disk/etc/shadow.tmp", "w");
    if (!out) {
        fclose(in);
        printf("passwd: cannot write /etc/shadow.tmp\n");
        return 1;
    }

    int found = 0;
    int authenticated = 0;
    char line[LINE_MAX];
    while (fgets(line, sizeof(line), in)) {
        const char *stored = (char *)0;
        const char *rest = (char *)0;

        if (user_matches(line, user, &stored, &rest)) {
            found = 1;
            if (password_equal(stored, rest, old_password)) {
                authenticated = 1;
                if (!write_updated_entry(out, user, new_password, rest)) {
                    fclose(in);
                    fclose(out);
                    unlink("/etc/shadow.tmp");
                    unlink("/disk/etc/shadow.tmp");
                    printf("passwd: failed to hash password\n");
                    return 1;
                }
            } else {
                fputs(line, out);
            }
        } else {
            fputs(line, out);
        }
    }

    fclose(in);
    fclose(out);

    if (!found) {
        unlink("/etc/shadow.tmp");
        unlink("/disk/etc/shadow.tmp");
        printf("passwd: unknown user %s\n", user);
        return 1;
    }

    if (!authenticated) {
        unlink("/etc/shadow.tmp");
        unlink("/disk/etc/shadow.tmp");
        printf("passwd: authentication failed\n");
        return 1;
    }

    if (rename("/etc/shadow.tmp", "/etc/shadow") != 0 &&
        rename("/disk/etc/shadow.tmp", "/disk/etc/shadow") != 0) {
        unlink("/etc/shadow.tmp");
        unlink("/disk/etc/shadow.tmp");
        printf("passwd: failed to update /etc/shadow\n");
        return 1;
    }

    printf("passwd: password updated for %s\n", user);
    return 0;
}
