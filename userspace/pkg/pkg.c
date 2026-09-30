#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include "indexsig.h"
#include "repo_pubkey.h"
#include "sha256.h"
#include "tarx.h"

/*
 * pkg — the MaeroOS package manager.
 *
 *   pkg update                  fetch the repo index
 *   pkg list                    show available + installed packages
 *   pkg install <name>          download + extract to /disk/apps/<name>/
 *   pkg remove <name>           delete an installed package
 *
 * Repo: http://<server>/index.txt + tars.  Server defaults to 10.0.2.2:8000
 * (the QEMU host); override in /disk/etc/pkg.conf with repo=host:port.
 *
 * The index is signed with the repo's Ed25519 key (tools/mkrepo.py; the
 * public half is compiled in as repo_pubkey.h), and pkg ignores any index,
 * fetched or cached, whose signature does not verify (indexsig.c).  `pkg
 * update` also refuses an index whose serial is lower than the cached one.
 * Every index line carries the tarball's SHA-256; install refuses a package
 * whose download does not match it, and extraction only accepts flat
 * archives of regular files (see tarx.c).
 */

#define INDEX_CACHE "/disk/etc/pkg-index.txt"
#define APPS_DIR    "/disk/apps"

static char repo_host[64] = "10.0.2.2";
static int repo_port = 8000;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void load_repo_conf(void) {
    char buf[256];
    int fd = open("/disk/etc/pkg.conf", O_RDONLY), n;

    if (fd < 0) return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    {
        char *p = strstr(buf, "repo=");
        if (p) {
            char *c;
            p += 5;
            c = strchr(p, ':');
            if (c) {
                *c = 0;
                strncpy(repo_host, p, sizeof(repo_host) - 1);
                repo_port = atoi(c + 1);
            }
        }
    }
}

/* HTTP GET path → malloc'd body in fetch_buf (caller frees); returns bytes
 * or -1.  Downloading to RAM keeps slow ext2 writes off the TCP critical path. */
#define MAX_DOWNLOAD (64 * 1024 * 1024)

static char *fetch_buf;
static int net_unreachable;   /* set when the repo server can't be reached */

static void fetch_fail(int fd) {
    if (fd >= 0) close(fd);
    free(fetch_buf);
    fetch_buf = 0;
}

/* "HTTP/1.x 200 ..." — look at the status code only, not at any "200" that
 * happens to appear in a header value. */
static int status_ok(const char *hdr) {
    const char *sp;
    if (strncmp(hdr, "HTTP/", 5)) return 0;
    sp = strchr(hdr, ' ');
    return sp && !strncmp(sp + 1, "200", 3) && (sp[4] == ' ' || sp[4] == '\r');
}

static int http_fetch_mem(const char *path, int expect, int show_progress) {
    struct sockaddr_in addr;
    char req[256];
    static char buf[16384];
    static char hdr[4096];
    int fd, n, total = 0, body = 0, idle = 0, hlen = 0;
    int cap = expect > 0 ? expect + 4096 : 256 * 1024;
    unsigned ip;

    fetch_buf = 0;
    if (expect < 0 || expect > MAX_DOWNLOAD) {
        printf("pkg: bad size %d for /%s\n", expect, path);
        return -1;
    }
    ip = resolve_a(repo_host);
    if (!ip) {
        printf("pkg: cannot resolve %s\n", repo_host);
        net_unreachable = 1;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)repo_port);
    addr.sin_addr.s_addr = ip;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("pkg: cannot reach %s:%d — on the host run: make repo-serve\n",
               repo_host, repo_port);
        net_unreachable = 1;
        close(fd);
        return -1;
    }
    fetch_buf = (char *)malloc((size_t)cap);
    if (!fetch_buf) { close(fd); return -1; }
    snprintf(req, sizeof(req),
             "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, repo_host);
    send(fd, req, strlen(req), 0);

    while (1) {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            const char *data = buf;
            int len = n;
            idle = 0;
            if (!body) {
                /* Collect headers (they may span reads) up to the blank line;
                 * whatever follows it in this read is the start of the body. */
                int before = hlen, take = n;
                char *e;
                if (take > (int)sizeof(hdr) - 1 - hlen) take = (int)sizeof(hdr) - 1 - hlen;
                memcpy(hdr + hlen, buf, (size_t)take);
                hlen += take;
                hdr[hlen] = 0;
                e = strstr(hdr, "\r\n\r\n");
                if (!e) {
                    if (hlen >= (int)sizeof(hdr) - 1) {
                        printf("pkg: bad response for /%s\n", path);
                        fetch_fail(fd);
                        return -1;
                    }
                    continue;
                }
                if (!status_ok(hdr)) {
                    printf("pkg: server refused /%s\n", path);
                    fetch_fail(fd);
                    return -1;
                }
                body = 1;
                data = buf + ((int)(e - hdr) + 4 - before);
                len = n - (int)(data - buf);
            }
            if (len > 0) {
                if (len > MAX_DOWNLOAD - total) {
                    printf("pkg: /%s is too large\n", path);
                    fetch_fail(fd);
                    return -1;
                }
                if (total + len > cap) {
                    char *nb;
                    while (total + len > cap) cap *= 2;
                    nb = (char *)realloc(fetch_buf, (size_t)cap);
                    if (!nb) { fetch_fail(fd); return -1; }
                    fetch_buf = nb;
                }
                memcpy(fetch_buf + total, data, (size_t)len);
                total += len;
            }
            if (show_progress && (total & 0x3FFFF) < 16384)
                printf("\r  %d KB", total / 1024);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN && ++idle < 3000) { sleep_ms(5); continue; }
        break;
    }
    close(fd);
    if (show_progress) printf("\r  %d KB received\n", total / 1024);
    if (!body || (expect > 0 && total != expect)) {
        printf("pkg: short download (%d of %d bytes) — try again\n",
               total, expect);
        fetch_fail(-1);
        return -1;
    }
    return total;
}

/* ── index handling ─────────────────────────────────────────────────────── */

typedef struct {
    char name[32], version[16], tar[48], caption[96], exec[96], args[96];
    char sha256[65];
    int size, fullscreen, rawinput;
} pkg_t;

static pkg_t pkgs[32];
static int pkg_count;

/* The index lives in a fixed buffer; pkg update refuses larger ones. */
#define INDEX_MAX 8191

static const char *index_error;   /* why the cached index was not trusted */

/* Read the cached index into buf (NUL-terminated); returns bytes or -1. */
static int read_cache(char *buf) {
    int fd = open(INDEX_CACHE, O_RDONLY), n = 0, r;

    if (fd < 0) return -1;
    while (n < INDEX_MAX && (r = read(fd, buf + n, INDEX_MAX - n)) > 0)
        n += r;
    close(fd);
    buf[n] = 0;
    return n;
}

static void parse_index(void) {
    static char buf[INDEX_MAX + 1];
    unsigned long serial;
    int n, len;
    char *line, *next;

    pkg_count = 0;
    index_error = 0;
    n = read_cache(buf);
    if (n <= 0) return;
    if (index_verify(buf, n, repo_pubkey, &serial, &len, &index_error))
        return;
    buf[len] = 0;   /* parse only what the signature covers */
    line = buf;
    while (line && *line && pkg_count < 32) {
        char *f[10] = {0};
        int nf = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (*line == '#') { line = next; continue; }
        f[nf++] = line;
        for (char *p = line; *p && nf < 10; p++)
            if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        /* Names end up in paths and URLs: skip lines that could escape. */
        if (nf >= 6 && pkg_name_ok(f[0]) && pkg_file_ok(f[3])) {
            pkg_t *pk = &pkgs[pkg_count++];
            strncpy(pk->name, f[0], sizeof(pk->name) - 1);
            strncpy(pk->version, f[1], sizeof(pk->version) - 1);
            pk->size = atoi(f[2]);
            strncpy(pk->tar, f[3], sizeof(pk->tar) - 1);
            strncpy(pk->caption, f[4], sizeof(pk->caption) - 1);
            strncpy(pk->exec, f[5], sizeof(pk->exec) - 1);
            pk->fullscreen = nf > 6 ? atoi(f[6]) : 0;
            if (nf > 7) strncpy(pk->args, f[7], sizeof(pk->args) - 1);
            pk->rawinput = nf > 8 ? atoi(f[8]) : 0;
            pk->sha256[0] = 0;
            if (nf > 9 && strlen(f[9]) == 64)
                strcpy(pk->sha256, f[9]);
        }
        line = next;
    }
}

static pkg_t *find_pkg(const char *name) {
    for (int i = 0; i < pkg_count; i++)
        if (!strcmp(pkgs[i].name, name)) return &pkgs[i];
    return 0;
}

static int installed(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), APPS_DIR "/%s/manifest", name);
    return access(path, 0) == 0;
}

/* Delete the files of an installed package, then its directory. */
static void remove_dir(const char *dir) {
    char path[192];
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

/* Write all of buf to a new file; 0 on success. */
static int write_file(const char *path, const char *buf, int n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644), off = 0;
    if (fd < 0) return -1;
    while (off < n) {
        int w = write(fd, buf + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += w;
    }
    return close(fd);
}

/* ── commands ───────────────────────────────────────────────────────────── */

/* Check a freshly fetched index: signed by the repo key, and not older than
 * the cached one (a replayed old index could hide security updates).
 * Returns 0 if it may replace the cache. */
static int index_acceptable(const char *buf, int n) {
    static char old[INDEX_MAX + 1];
    unsigned long serial, old_serial;
    const char *why;
    int len, on;

    if (n > INDEX_MAX) {
        printf("pkg: index is too large — index rejected\n");
        return -1;
    }
    if (index_verify(buf, n, repo_pubkey, &serial, &len, &why)) {
        printf("pkg: %s — index rejected\n", why);
        return -1;
    }
    on = read_cache(old);
    if (on > 0 && !index_verify(old, on, repo_pubkey, &old_serial, &len, &why)
        && serial < old_serial) {
        printf("pkg: index serial %lu is older than the cached %lu — "
               "refusing rollback\n", serial, old_serial);
        return -1;
    }
    return 0;
}

static int cmd_update(void) {
    int n;

    mkdir("/disk/etc", 0755);
    n = http_fetch_mem("index.txt", 0, 0);
    if (n < 0) return net_unreachable ? 2 : 1;
    if (index_acceptable(fetch_buf, n)) {
        free(fetch_buf);
        fetch_buf = 0;
        return 1;
    }
    if (write_file(INDEX_CACHE, fetch_buf, n)) {
        printf("pkg: cannot write index cache\n");
        free(fetch_buf);
        fetch_buf = 0;
        return 1;
    }
    free(fetch_buf);
    fetch_buf = 0;
    parse_index();
    printf("pkg: %d package(s) available\n", pkg_count);
    return 0;
}

static int cmd_list(void) {
    parse_index();
    if (index_error) {
        printf("pkg: cached index not trusted: %s — run `pkg update`\n",
               index_error);
        return 1;
    }
    if (!pkg_count) {
        printf("pkg: no index — run `pkg update` first\n");
        return 1;
    }
    for (int i = 0; i < pkg_count; i++)
        printf("%c %-12s %-8s %s\n", installed(pkgs[i].name) ? '*' : ' ',
               pkgs[i].name, pkgs[i].version, pkgs[i].caption);
    printf("(* = installed)\n");
    return 0;
}

static int cmd_install(const char *name) {
    pkg_t *pk;
    char dir[128], mpath[140], sum[65], m[320];
    const char *why;
    int n, files;

    if (!pkg_name_ok(name)) {
        printf("pkg: invalid package name '%s'\n", name);
        return 1;
    }
    parse_index();
    if (index_error) {
        printf("pkg: cached index not trusted: %s — run `pkg update`\n",
               index_error);
        return 1;
    }
    pk = find_pkg(name);
    if (!pk) {
        printf("pkg: unknown package '%s' (run `pkg update`)\n", name);
        return 1;
    }
    if (!pk->sha256[0] || pk->size <= 0) {
        printf("pkg: index has no checksum for %s — run `pkg update`\n", name);
        return 1;
    }
    printf("Installing %s %s (%d KB)...\n", pk->name, pk->version,
           pk->size / 1024);

    n = http_fetch_mem(pk->tar, pk->size, 1);
    if (n < 0) return net_unreachable ? 2 : 1;
    sha256_hex(fetch_buf, (size_t)n, sum);
    for (char *p = pk->sha256; *p; p++)
        if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    if (strcmp(sum, pk->sha256)) {
        printf("pkg: checksum mismatch for %s — download rejected\n", name);
        free(fetch_buf);
        fetch_buf = 0;
        return 1;
    }

    mkdir(APPS_DIR, 0755);
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", pk->name);
    mkdir(dir, 0755);
    printf("Writing %d KB to disk...\n", n / 1024);
    files = untar_mem(fetch_buf, n, dir, &why);
    free(fetch_buf);
    fetch_buf = 0;
    if (files <= 0) {
        printf("pkg: extraction failed: %s\n", why);
        remove_dir(dir);
        return 1;
    }

    /* manifest drives the desktop launcher */
    snprintf(mpath, sizeof(mpath), "%s/manifest", dir);
    n = snprintf(m, sizeof(m),
                 "name=%s\nversion=%s\nexec=%s\nfullscreen=%d\n"
                 "args=%s\nrawinput=%d\n",
                 pk->name, pk->version, pk->exec, pk->fullscreen,
                 pk->args, pk->rawinput);
    if (n >= (int)sizeof(m) || write_file(mpath, m, n)) {
        printf("pkg: cannot write %s\n", mpath);
        remove_dir(dir);
        return 1;
    }
    printf("Installed %s (%d files) → %s\n", pk->name, files, dir);
    return 0;
}

static int cmd_remove(const char *name) {
    char dir[128];
    DIR *d;

    if (!pkg_name_ok(name)) {
        printf("pkg: invalid package name '%s'\n", name);
        return 1;
    }
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", name);
    d = opendir(dir);
    if (!d) { printf("pkg: '%s' is not installed\n", name); return 1; }
    closedir(d);
    remove_dir(dir);
    printf("Removed %s\n", name);
    return 0;
}

int main(int argc, char **argv) {
    load_repo_conf();
    if (argc >= 2 && !strcmp(argv[1], "update")) return cmd_update();
    if (argc >= 2 && !strcmp(argv[1], "list")) return cmd_list();
    if (argc >= 3 && !strcmp(argv[1], "install")) return cmd_install(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "remove")) return cmd_remove(argv[2]);
    printf("usage: pkg update | list | install <name> | remove <name>\n");
    return 1;
}
