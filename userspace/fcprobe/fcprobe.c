/* fcprobe — reproduce fontconfig's cache-dir writability test as the desktop
 * user (uid 1000), to find why "No writable cache directories" persists.
 * Mirrors src/fccache.c FcCacheWrite: access(W_OK) → else mkdir → CACHEDIR.TAG,
 * plus the atomic temp-file+rename dance and reading our config. */
#include "../include/fcntl.h"
#include "../include/stdio.h"
#include "../include/unistd.h"
#include "../include/sys/stat.h"

#define W_OK 2
#define F_OK 0

static void test_cachedir(const char *d) {
    printf("--- cachedir %s ---\n", d);
    int aw = access(d, W_OK);
    printf("  access(W_OK)=%d\n", aw);
    int af = access(d, F_OK);
    printf("  access(F_OK)=%d\n", af);
    int m = mkdir(d, 0755);
    printf("  mkdir=%d\n", m);
    char tag[256], tmp[256], done[256];
    int n = 0; while (d[n]) n++;
    /* CACHEDIR.TAG */
    for (int i=0;i<=n;i++) tag[i]=d[i]; tag[n]=0;
    printf("  (after mkdir) access(W_OK)=%d\n", access(d, W_OK));
    /* atomic write probe: create temp, write, rename */
    int i=0; for(;d[i];i++) tmp[i]=d[i]; const char *t="/probe.tmp"; for(int j=0;t[j];j++) tmp[i++]=t[j]; tmp[i]=0;
    i=0; for(;d[i];i++) done[i]=d[i]; const char *u="/probe.done"; for(int j=0;u[j];j++) done[i++]=u[j]; done[i]=0;
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    printf("  open(%s,O_CREAT)=%d\n", tmp, fd);
    if (fd >= 0) { write(fd, "x", 1); close(fd); }
    int r = rename(tmp, done);
    printf("  rename(tmp->done)=%d\n", r);
    (void)tag;
}

int main(void) {
    /* read our config first (as root, before dropping) */
    int cf = open("/etc/fonts/fonts.conf", O_RDONLY);
    printf("fcprobe: open /etc/fonts/fonts.conf = %d\n", cf);
    if (cf >= 0) close(cf);

    /* drop to the desktop user, like the desktop's run_program_as_user does */
    int sg = setgid(1000);
    int su = setuid(1000);
    printf("fcprobe: setgid(1000)=%d setuid(1000)=%d uid=%d\n", sg, su, getuid());

    test_cachedir("/tmp/fontcache");    /* our fonts.conf <cachedir> */
    test_cachedir("/tmp/fontconfig");   /* XDG_CACHE_HOME(/tmp)/fontconfig */

    printf("fcprobe done\n");
    return 0;
}
