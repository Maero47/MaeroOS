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

/* HTTP GET path → malloc'd body (caller frees); returns bytes or -1.
 * Downloading to RAM keeps slow ext2 writes off the TCP critical path. */
static char *fetch_buf;
static int net_unreachable;   /* set when the repo server can't be reached */

static int http_fetch_mem(const char *path, int expect, int show_progress) {
    struct sockaddr_in addr;
    char req[256];
    static char buf[16384];
    int fd, n, total = 0, body = 0, idle = 0;
    int cap = expect > 0 ? expect + 4096 : 256 * 1024;
    unsigned ip = resolve_a(repo_host);

    fetch_buf = (char *)malloc((size_t)cap);
    if (!fetch_buf) return -1;

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
    snprintf(req, sizeof(req),
             "GET /%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             path, repo_host);
    send(fd, req, strlen(req), 0);

    while (1) {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            int off = 0;
            idle = 0;
            if (!body) {
                /* find the end of headers (may span reads — keep simple:
                 * headers always fit the first read from python http) */
                char *b = NULL;
                buf[n > 0 ? n : 0] = 0;
                b = strstr(buf, "\r\n\r\n");
                if (!b) continue;
                if (!strstr(buf, "200")) {
                    printf("pkg: server error for /%s\n", path);
                    close(fd);
                    return -1;
                }
                off = (int)(b - buf) + 4;
                body = 1;
            }
            if (total + (n - off) > cap) {       /* grow if index etc */
                int ncap = cap * 2;
                char *nb = (char *)malloc((size_t)ncap);
                if (!nb) { free(fetch_buf); fetch_buf = 0; close(fd); return -1; }
                memcpy(nb, fetch_buf, (size_t)total);
                free(fetch_buf);
                fetch_buf = nb;
                cap = ncap;
            }
            memcpy(fetch_buf + total, buf + off, (size_t)(n - off));
            total += n - off;
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
    if (expect > 0 && total != expect) {
        printf("pkg: short download (%d of %d bytes) — try again\n",
               total, expect);
        free(fetch_buf);
        fetch_buf = 0;
        return -1;
    }
    return total;
}

/* ── index handling ─────────────────────────────────────────────────────── */

typedef struct {
    char name[32], version[16], tar[48], caption[96], exec[96], args[96];
    int size, fullscreen, rawinput;
} pkg_t;

static pkg_t pkgs[32];
static int pkg_count;

static void parse_index(void) {
    static char buf[8192];
    int fd = open(INDEX_CACHE, O_RDONLY), n;
    char *line, *next;

    pkg_count = 0;
    if (fd < 0) return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    line = buf;
    while (line && *line && pkg_count < 32) {
        char *f[9] = {0};
        int nf = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        f[nf++] = line;
        for (char *p = line; *p && nf < 9; p++)
            if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        if (nf >= 6) {
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

/* ── ustar extraction ───────────────────────────────────────────────────── */

static unsigned octal(const char *s, int len) {
    unsigned v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (unsigned)(s[i] - '0');
    return v;
}

static int untar(const char *tar_path, const char *dest_dir) {
    static char hdr[512], buf[8192];
    int fd = open(tar_path, O_RDONLY);
    int files = 0;

    if (fd < 0) return -1;
    while (read(fd, hdr, 512) == 512) {
        char *name = hdr;
        unsigned size, mode;
        char out_path[160];
        int out;

        if (!name[0]) break;
        if (memcmp(hdr + 257, "ustar", 5)) break;
        size = octal(hdr + 124, 12);
        mode = octal(hdr + 100, 8) & 0777;   /* ustar mode field */
        if (hdr[156] != '0' && hdr[156] != 0) {   /* skip non-files */
            unsigned skip = (size + 511) & ~511U;
            lseek(fd, (int)skip, SEEK_CUR);
            continue;
        }
        /* strip leading ./ */
        if (name[0] == '.' && name[1] == '/') name += 2;
        snprintf(out_path, sizeof(out_path), "%s/%s", dest_dir, name);
        out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            printf("pkg: cannot write %s\n", out_path);
            close(fd);
            return -1;
        }
        {
            unsigned left = size;
            while (left > 0) {
                int want = left > sizeof(buf) ? (int)sizeof(buf) : (int)left;
                int n = read(fd, buf, want);
                if (n <= 0) break;
                write(out, buf, n);
                left -= (unsigned)n;
            }
        }
        close(out);
        /* Honour the tar entry's permission bits so executables stay
         * executable (open(O_CREAT) alone yields 0644 → exec denied). */
        if (mode & 0111)
            chmod(out_path, mode ? mode : 0755);
        files++;
        /* advance to the next 512 boundary */
        if (size % 512)
            lseek(fd, (int)(512 - size % 512), SEEK_CUR);
    }
    close(fd);
    return files;
}

/* ── commands ───────────────────────────────────────────────────────────── */

static int cmd_update(void) {
    int fd;

    int n;

    mkdir("/disk/etc", 0755);
    n = http_fetch_mem("index.txt", 0, 0);
    if (n < 0) return net_unreachable ? 2 : 1;
    fd = open(INDEX_CACHE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("pkg: cannot write index cache\n"); free(fetch_buf); return 1; }
    write(fd, fetch_buf, n);
    close(fd);
    free(fetch_buf);
    fetch_buf = 0;
    parse_index();
    printf("pkg: %d package(s) available\n", pkg_count);
    return 0;
}

static int cmd_list(void) {
    parse_index();
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
    char tar_path[128], dir[128], mpath[140];
    int fd, files;

    parse_index();
    pk = find_pkg(name);
    if (!pk) {
        printf("pkg: unknown package '%s' (run `pkg update`)\n", name);
        return 1;
    }
    printf("Installing %s %s (%d KB)...\n", pk->name, pk->version,
           pk->size / 1024);
    mkdir(APPS_DIR, 0755);
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", pk->name);
    mkdir(dir, 0755);
    snprintf(tar_path, sizeof(tar_path), "%s/.download.tar", dir);

    {
        int n = http_fetch_mem(pk->tar, pk->size, 1);
        if (n < 0) return net_unreachable ? 2 : 1;
        printf("Writing %d KB to disk...\n", n / 1024);
        fd = open(tar_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("pkg: cannot write to %s\n", dir);
            free(fetch_buf);
            fetch_buf = 0;
            return 1;
        }
        {
            int off = 0;
            while (off < n) {
                int w = write(fd, fetch_buf + off, n - off);
                if (w <= 0) break;
                off += w;
            }
        }
        close(fd);
        free(fetch_buf);
        fetch_buf = 0;
    }

    files = untar(tar_path, dir);
    unlink(tar_path);
    if (files <= 0) { printf("pkg: extraction failed\n"); return 1; }

    /* manifest drives the desktop launcher */
    snprintf(mpath, sizeof(mpath), "%s/manifest", dir);
    fd = open(mpath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        char m[320];
        int n = snprintf(m, sizeof(m),
                         "name=%s\nversion=%s\nexec=%s\nfullscreen=%d\n"
                         "args=%s\nrawinput=%d\n",
                         pk->name, pk->version, pk->exec, pk->fullscreen,
                         pk->args, pk->rawinput);
        write(fd, m, n);
        close(fd);
    }
    printf("Installed %s (%d files) → %s\n", pk->name, files, dir);
    return 0;
}

static int cmd_remove(const char *name) {
    char dir[128], path[192];
    DIR *d;
    struct dirent *e;

    snprintf(dir, sizeof(dir), APPS_DIR "/%s", name);
    d = opendir(dir);
    if (!d) { printf("pkg: '%s' is not installed\n", name); return 1; }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
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
