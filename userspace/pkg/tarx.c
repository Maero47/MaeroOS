#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tarx.h"

static int name_chars_ok(const char *s, int max) {
    int n = 0;
    if (!s || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
                (s[0] >= '0' && s[0] <= '9')))
        return 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= max) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int pkg_name_ok(const char *name) { return name_chars_ok(name, 31); }
int pkg_file_ok(const char *name) { return name_chars_ok(name, 47); }

int tar_member_ok(const char *name) {
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (const char *p = name; *p; p++)
        if (*p == '/' || (unsigned char)*p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

/* Parse a NUL/space-terminated octal field; -1 if it holds anything else. */
static long octal(const char *s, int len) {
    long v = 0;
    int i = 0, digits = 0;
    while (i < len && s[i] == ' ') i++;
    for (; i < len && s[i] >= '0' && s[i] <= '7'; i++, digits++) {
        if (v > 0x0fffffffL) return -1;
        v = v * 8 + (s[i] - '0');
    }
    for (; i < len; i++)
        if (s[i] != 0 && s[i] != ' ') return -1;
    return digits ? v : -1;
}

static int header_sum_ok(const unsigned char *h) {
    long want = octal((const char *)h + 148, 8);
    long sum = 0;
    for (int i = 0; i < 512; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return want >= 0 && sum == want;
}

static int all_zero(const char *p, int n) {
    for (int i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* Check every header up front so a bad archive writes nothing at all. */
static int scan(const char *data, int len, const char **why) {
    int off = 0, files = 0;
    while (off + 512 <= len) {
        const char *h = data + off;
        char name[101];
        long size;

        if (all_zero(h, 512)) return files;          /* end-of-archive block */
        if (memcmp(h + 257, "ustar", 5)) { *why = "not a ustar archive"; return -1; }
        if (!header_sum_ok((const unsigned char *)h)) { *why = "bad header checksum"; return -1; }
        if (h[345]) { *why = "long (prefixed) member name"; return -1; }
        memcpy(name, h, 100);                        /* the field need not be */
        name[100] = 0;                               /* NUL-terminated        */
        if (h[156] != '0' && h[156] != 0) { *why = "member is not a regular file"; return -1; }
        {
            const char *n = name;
            if (n[0] == '.' && n[1] == '/') n += 2;
            if (!tar_member_ok(n)) { *why = "unsafe member name"; return -1; }
        }
        size = octal(h + 124, 12);
        if (size < 0 || size > len - off - 512) { *why = "truncated archive"; return -1; }
        off += 512 + (int)((size + 511) & ~511L);
        files++;
    }
    *why = "missing end-of-archive marker";
    return -1;
}

int untar_mem(const char *data, int len, const char *dest_dir, const char **why) {
    int off = 0, files = 0;

    *why = "";
    if (scan(data, len, why) <= 0) {
        if (!**why) *why = "empty archive";
        return -1;
    }
    while (off + 512 <= len && !all_zero(data + off, 512)) {
        const char *h = data + off, *n;
        char name[101], out_path[256];
        long size = octal(h + 124, 12);
        long mode = octal(h + 100, 8);
        int out, w;

        memcpy(name, h, 100);
        name[100] = 0;
        n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        if ((int)strlen(dest_dir) + 1 + (int)strlen(n) >= (int)sizeof(out_path)) {
            *why = "path too long";
            return -1;
        }
        strcpy(out_path, dest_dir);
        strcat(out_path, "/");
        strcat(out_path, n);

        /* Never write through whatever already sits at this name (a symlink
         * left in the directory would redirect the write elsewhere). */
        unlink(out_path);
        out = open(out_path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
                   (mode >= 0 && (mode & 0111)) ? 0755 : 0644);
        if (out < 0) { *why = "cannot create file"; return -1; }
        for (long done = 0; done < size; done += w) {
            w = write(out, h + 512 + done, (int)(size - done));
            if (w <= 0) { close(out); *why = "write failed"; return -1; }
        }
        if (close(out) < 0) { *why = "write failed"; return -1; }
        files++;
        off += 512 + (int)((size + 511) & ~511L);
    }
    return files;
}
