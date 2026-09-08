/* /disk/ff — one-command Firefox launcher for the MaeroOS desktop terminal.
 *
 * Starts a windowed maeroX X server in a desktop slot, sets up a writable
 * profile in /tmp, and execs Firefox into it.  Built as a real binary (not a
 * shell script) so it runs when typed directly — the MaeroOS shell only runs
 * scripts when sourced (`. file`), which is easy to get wrong.
 *
 * Firefox needs LD_LIBRARY_PATH (its libs live in /disk/firefox) and DISPLAY
 * (the maeroX server).  We pass an explicit environment so it works regardless
 * of how the parent shell was set up.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Ask the kernel to dump its cycle-accounting counters (kprof, syscall 503) at
 * the instant the paint marker appears, so the profile covers exactly the
 * startup being measured rather than the nearest periodic dump. */
static void kprof_mark(void) {
    __asm__ volatile("int $0x80" :: "a"(503) : "memory");
}

static char *const ff_envp[] = {
    "PATH=/disk:/disk/bin:/:/bin",
    /* HOME must be WRITABLE: nsToolkitProfileService creates/writes
     * $HOME/.mozilla/firefox/{profiles.ini,installs.ini} during startup even when
     * -profile is given.  /home is on the read-only initrd (mkdir $HOME/.mozilla
     * → EPERM), so the profile service init fails → "Your Firefox profile cannot
     * be loaded. It may be missing or inaccessible." modal that hangs ShowModal.
     * Point HOME at the writable tmpfs (created by the launcher below). */
    "HOME=/tmp/ffhome",
    "USER=user",
    "TERM=linux",
    "LD_LIBRARY_PATH=/lib:/disk/lib:/disk/firefox",
    "DISPLAY=:0",
    "GDK_PIXBUF_MODULE_FILE=/disk/firefox/pixbuf-loaders/loaders.cache",
    "XDG_CACHE_HOME=/tmp",      /* fontconfig cache (avoids "no writable cache") */
    /* The GTK icon theme (/usr/share/icons/hicolor) lives on disk-ff, mounted at
     * /disk → at runtime it's /disk/usr/share/icons.  GTK only searches
     * $XDG_DATA_DIRS/icons; without this it checks /usr/share/icons (initrd,
     * empty — icons are excluded there) and falls back to libgtk's builtin
     * gresource hicolor, whose window-minimize-symbolic.symbolic.png fails to
     * load → NULL pixbuf → crash.  Point GTK at the on-disk theme. */
    "XDG_DATA_DIRS=/disk/usr/share:/usr/share:/usr/local/share",
    /* Force GTK's ICON theme to "hicolor" (our on-disk theme that has the window-
     * control symbolic icons).  By default GTK's icon theme is "Adwaita"/builtin,
     * which isn't on disk → GTK loads the window-minimize-symbolic icon from its
     * compiled-in gresource fallback, which fails to decode → NULL pixbuf →
     * crash.  settings.ini (read from $XDG_CONFIG_HOME/gtk-3.0/) points GTK at
     * hicolor so it finds our on-disk icons instead. */
    "XDG_CONFIG_HOME=/disk/ffcfg",
    /* Disable GTK client-side decorations.  Firefox's CSD titlebar makes GTK
     * load its built-in symbolic button icons (window-minimize-symbolic etc.)
     * from libgtk's gresource — that load FAILS here ("Could not load a pixbuf
     * from /org/gtk/libgtk/icons/.../window-minimize-symbolic.symbolic.png") →
     * NULL pixbuf → crash before paint.  With CSD off, maeroX (the WM) draws the
     * titlebar and GTK never touches those icons, removing the pre-paint crash. */
    "GTK_CSD=0",
    /* Disable the GTK AT-SPI accessibility bridge: it loads lazily at window
     * realize and tries to reach a D-Bus session bus that doesn't exist here,
     * blocking window creation (proven via gtkprobe: hangs right after GTK init,
     * during show).  Standard fix for GTK apps on minimal/headless systems. */
    "NO_AT_BRIDGE=1",
    "GTK_A11Y=none",
    /* Firefox's child processes try to install a seccomp-bpf + namespace sandbox
     * that this kernel doesn't provide; without disabling it the content/RDD/GMP
     * children bail immediately and the parent stalls waiting for them. */
    "MOZ_DISABLE_CONTENT_SANDBOX=1",
    /* Crash reporter (Breakpad) LEFT ENABLED: with the clone-child-stack bug
     * fixed its dumper works, so when a CHILD process (content/GPU) crashes
     * Breakpad contains it and the PARENT keeps running — that's how the early
     * run reached paint (putimg=24).  MOZ_CRASHREPORTER_DISABLE=1 made every
     * crash fatal (status 139) and dropped the paint rate to ~0, so it's OUT. */
    "MOZ_DISABLE_GMP_SANDBOX=1",
    "MOZ_DISABLE_RDD_SANDBOX=1",
    "MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1",
    "MOZ_DISABLE_UTILITY_SANDBOX=1",
    /* NB (2026-07-03): MOZ_FORCE_DISABLE_E10S=1 RE-TESTED with all the new fixes
     * (profile/inotify/EEXIST) — does NOT help.  FF115 STILL launches a content
     * process for the tab even with e10s "disabled" (main thread still parks in
     * ContentParent::GetNewOrUsedLaunchingBrowserProcess → WaitForProcessHandle,
     * and it reaches the 1152x720 resize).  So there is no way to avoid the
     * content-process launch; the only path to paint is making WaitForProcessHandle
     * complete.  e10s stays enabled. */
    "FONTCONFIG_PATH=/etc/fonts",  /* libfontconfig was built with prefix=/sysroot,
                                    * so its compiled config dir is /sysroot/etc/fonts
                                    * (absent here).  Point it at our real config,
                                    * which declares <cachedir>/tmp/fontcache</cachedir>. */
    /* Diagnostic: make Firefox report compositor/widget/webrender bring-up to
     * stderr (→ /dev/tty → host serial) so we can see WHERE first paint stalls. */
    /* Diagnostic: make Firefox report compositor/widget/webrender bring-up to
     * stderr (→ /dev/tty → host serial) so we can see WHERE first paint stalls.
     * (events:5/chromium:5 used transiently to confirm the IPC I/O Parent thread
     * never runs the launch task — see firefox-paint-blocker memory.) */
    /* nsDocShell:5 (release-available LazyLogModule) logs "DoURILoad"/"InternalLoad"
     * with the URI of every document load, revealing the chrome URL of the startup
     * modal that AppWindow::ShowModal spins on (symbolized backtrace: chrome JS
     * opens it via nsWindowWatcher::OpenWindowInternal before the browser window). */
    /* NB: the old "DocShell:4" matched no log module at all (the module is
     * called "nsDocShell"), and a control run of this same binary on a Linux
     * host showed that "nsDocShell:5" prints nothing on the startup path
     * either — so the absence of document-load lines in earlier runs was never
     * evidence of anything.  The default here is deliberately lean: Widget:5
     * covers the whole window lifecycle (Create / Resize / SetSizeMode / Show /
     * NativeShow / OnMap / the GtkCompositorWidget), which is what a paint
     * regression needs.  Heavier modules are switched on per run by writing
     * /disk/ffcfg/ffmozlog (see build_env below) — no rebuild needed.  The two
     * that cracked the never-shown-window bug were:
     *   LoadGroup:5      names every request added to and removed from a
     *                    document's load group; the chrome window is shown only
     *                    once browser.xhtml's group drains
     *                    (AppWindow::OnStateChange -> OnChromeLoaded ->
     *                    SetVisibility -> nsWindow::Show)
     *   nsJarProtocol:5  every stage of an omni.ja channel, which showed the
     *                    icon loads stopping at CreateLocalJarInput */
    "MOZ_LOG=sync,timestamp,Widget:5",
    /* Route Firefox's own MOZ_LOG to a file (stderr→/dev/tty routing doesn't reach
     * serial reliably).  The watchdog dumps this file's TAIL to stdout on a stall,
     * so we can see WHERE the parent stalls (e.g. never creating the main XUL
     * browser window).  FF appends ".child-PID" for child processes; the base name
     * is the PARENT (the one that creates the toplevel window we care about). */
    "MOZ_LOG_FILE=/tmp/moz.log",
    (char *)0
};

/* Firefox's environment, with the MOZ_LOG entry overridable at runtime from
 * /disk/ffcfg/ffmozlog.  Diagnosing the startup stall means changing which log
 * modules are on, and rewriting a 6-byte file inside disk-ff.img is a great
 * deal cheaper than rebuilding the 1 GiB image for every experiment. */
static char  moz_log_buf[512];
static char *ff_env[sizeof(ff_envp) / sizeof(ff_envp[0])];

static char *const *build_env(void) {
    unsigned n = 0;
    for (; ff_envp[n]; n++) ff_env[n] = ff_envp[n];
    ff_env[n] = 0;

    int fd = open("/disk/ffcfg/ffmozlog", O_RDONLY);
    if (fd < 0) return ff_env;
    int r = read(fd, moz_log_buf + 8, sizeof(moz_log_buf) - 9);
    close(fd);
    if (r <= 0) return ff_env;
    memcpy(moz_log_buf, "MOZ_LOG=", 8);
    r += 8;
    while (r > 8 && (moz_log_buf[r - 1] == '\n' || moz_log_buf[r - 1] == '\r'))
        r--;
    moz_log_buf[r] = '\0';
    for (unsigned i = 0; i < n; i++)
        if (strncmp(ff_env[i], "MOZ_LOG=", 8) == 0) {
            ff_env[i] = moz_log_buf;
            printf("ff: %s\n", moz_log_buf);
        }
    return ff_env;
}

static void copyfile(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { printf("ff: cannot write %s\n", dst); close(in); return; }
    char buf[4096];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
}

/* Copy the last `maxbytes` of src into dst.  Used to park MOZ_LOG tails on the
 * ext2 disk (see save_moz_logs) where the host can read them afterwards. */
static void copytail(const char *src, const char *dst, int maxbytes) {
    int in = open(src, O_RDONLY);
    if (in < 0) return;
    int sz  = lseek(in, 0, SEEK_END);
    int off = (sz > maxbytes) ? sz - maxbytes : 0;
    lseek(in, off, SEEK_SET);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { printf("ff: cannot write %s\n", dst); close(in); return; }
    char buf[4096];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
    printf("ff: saved %s (%d of %d bytes)\n", dst, sz - off, sz);
}

/* Save the parent's MOZ_LOG *and* every child's (Firefox appends
 * ".child-N.moz_log" for child processes) under /disk/ffout/aNN-<name>.
 * /disk is the ext2 image, so the host can read the complete logs out of
 * disk-ff.img with debugfs after the run instead of paying serial bandwidth
 * for them.  Silently does nothing when the directory cannot be created. */
static void save_moz_logs(int attempt) {
    mkdir("/disk/ffout", 0777);
    DIR *d = opendir("/tmp");
    if (!d) { printf("ff: cannot read /tmp\n"); return; }
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (strncmp(e->d_name, "moz.log", 7) != 0) continue;
        char src[288], dst[320];
        snprintf(src, sizeof(src), "/tmp/%s", e->d_name);
        snprintf(dst, sizeof(dst), "/disk/ffout/a%02d-%s", attempt, e->d_name);
        copytail(src, dst, 512 * 1024);
    }
    closedir(d);
}

/* Watchdog policy, overridable from /disk/ffcfg/ffwatch so a diagnostic run can
 * be re-tuned by rewriting one small file in disk-ff.img (debugfs) instead of
 * rebuilding the 1 GiB image.  Format: "<max attempts> <per-attempt seconds>". */
static void read_watch_cfg(int *max_attempts, int *timeout_s) {
    int fd = open("/disk/ffcfg/ffwatch", O_RDONLY);
    if (fd < 0) return;
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int a = 0, t = 0;
    if (sscanf(buf, "%d %d", &a, &t) == 2 && a > 0 && t > 0) {
        *max_attempts = a;
        *timeout_s    = t;
        printf("ff: watchdog from /disk/ffcfg/ffwatch: %d attempts, %ds each\n", a, t);
    }
}

/* Dump the last `maxbytes` of a file to stdout (→ serial).  Used to surface
 * Firefox's own MOZ_LOG tail on a stall without flooding the slow serial line. */
static void dump_tail(const char *path, int maxbytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("ff: (no %s)\n", path); return; }
    int sz = lseek(fd, 0, SEEK_END);
    int off = (sz > maxbytes) ? sz - maxbytes : 0;
    lseek(fd, off, SEEK_SET);
    printf("=== tail(%s) from %d/%d ===\n", path, off, sz);
    char buf[2048];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    printf("\n=== end tail ===\n");
    close(fd);
}

int main(void) {
    char *const *envp = build_env();
    printf("Starting maeroX X server in a desktop slot...\n");
    int pid = fork();
    if (pid == 0) {
        char *a[] = { "/disk/maerox", "3", "-D", (char *)0 };
        execve(a[0], a, envp);
        _exit(127);
    }
    usleep(1000000);   /* give maeroX ~1s to start listening */
    usleep(1000000);

    mkdir("/tmp/ffhome", 0755);      /* writable $HOME for .mozilla/profiles.ini */
    mkdir("/tmp/fontcache", 0777);   /* fonts.conf <cachedir>; fontconfig needs
                                      * it to exist + be writable, else it warns
                                      * "No writable cache directories". */

    printf("Launching Firefox 115...\n");
    /* about:blank — the configuration proven to let the BROWSER itself paint its
     * chrome (the data: content path engages the content process which hits the
     * intermittent icon crash more reliably before the window paints).  With the
     * glyph + full-size + PNG-loader work, a surviving paint now shows full chrome
     * WITH text. */
    char *a[] = { "/disk/firefox/firefox-bin", "-profile", "/tmp/ffp",
                  "-no-remote", "about:blank", (char *)0 };

    /* Watchdog restart: launch Firefox, wait up to PAINT_TIMEOUT_S for maeroX to
     * drop /tmp/ff_painted (first PutImage); if it paints, keep running; if it
     * crashes or stalls, kill the tree and relaunch.
     *
     * The timeout has to be a WALL-CLOCK deadline, not a count of usleep(100 ms)
     * iterations.  On this kernel each loop iteration (waitpid + access + usleep)
     * costs 250-450 ms while Firefox is loading, so the old "400 x 100 ms = 40 s"
     * loop actually ran for 110-180 s and its printed "40s" was fiction.
     *
     * The 40 s budget itself was the blocker.  Measured on 2026-09-08
     * (build/ff-smoke/20260908-010602-baseline20): the parent needs ~110 s from
     * exec to `nsWindow::Create() Toplevel`, another ~60 s of chrome-document
     * load, and only THEN does AppWindow::OnChromeLoaded() run SizeShell() +
     * SetVisibility(true), which is what calls nsWindow::Show(true) and finally
     * maps the X window.  All three attempts of that run reached SizeShell
     * ("Resize 736 x 477" / "SetSizeMode 2 / set maximized") and were SIGKILLed
     * within 0-6 s of it, i.e. a couple of log lines short of the MapWindow.
     * So: far fewer attempts, each long enough to finish startup. */
    int MAX_ATTEMPTS = 6;
    int PAINT_TIMEOUT_S = 300;
    read_watch_cfg(&MAX_ATTEMPTS, &PAINT_TIMEOUT_S);
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        unlink("/tmp/ff_painted");              /* reset the paint marker */

        /* Recreate a CLEAN profile each attempt: rm the dir (best-effort via the
         * few files FF writes) then re-copy prefs, so a watchdog-killed attempt's
         * stale lock/startupCache/session don't poison the next.  Single dir (no
         * per-attempt numbering — that pressured tmpfs and caused status=1 early
         * exits across 20 attempts). */
        mkdir("/tmp/ffp", 0755);
        unlink("/tmp/ffp/.parentlock");
        unlink("/tmp/ffp/lock");
        copyfile("/disk/ffprofile/prefs.js", "/tmp/ffp/prefs.js");
        copyfile("/disk/ffprofile/user.js",  "/tmp/ffp/user.js");

        int fpid = fork();
        if (fpid == 0) {
            int tty = open("/dev/tty", O_WRONLY);   /* GTK errors → host serial */
            if (tty >= 0) { dup2(tty, 1); dup2(tty, 2); if (tty > 2) close(tty); }
            execve(a[0], a, envp);
            _exit(127);
        }
        if (fpid < 0) return 1;

        int status = 0, painted = 0, exited = 0;
        time_t deadline = time((time_t *)0) + PAINT_TIMEOUT_S;
        /* Iteration cap as a backstop in case the clock ever misbehaves: each
         * pass costs >= 100 ms of usleep, so this can only fire after the
         * deadline would have. */
        int max_iters = PAINT_TIMEOUT_S * 20;
        for (int t = 0; t < max_iters && time((time_t *)0) < deadline; t++) {
            if (waitpid(fpid, &status, 1 /* WNOHANG */) == fpid) { exited = 1; break; }
            if (access("/tmp/ff_painted", F_OK) == 0) { kprof_mark(); painted = 1; break; }
            usleep(100000);   /* 100 ms */
        }

        if (painted) {
            /* The marker means SOMETHING painted — but if the browser process
             * just crashed, Breakpad's crash-reporter dialog is what painted it,
             * not the browser.  Grace-check: watch fpid for a few seconds; if it
             * exits with a crash status now, that paint was the crash reporter →
             * retry.  Only a browser that stays alive counts as a real window. */
            int crashed_after_paint = 0;
            for (int g = 0; g < 50; g++) {       /* 5 s grace */
                if (waitpid(fpid, &status, 1 /* WNOHANG */) == fpid) {
                    if (status != 0) crashed_after_paint = 1;
                    break;
                }
                usleep(100000);
            }
            if (!crashed_after_paint) {
                printf("ff: Firefox painted — window is up (attempt %d).\n", attempt + 1);
                waitpid(fpid, &status, 0);      /* now run until the user quits */
                return 0;
            }
            printf("ff: paint was the crash reporter (browser crashed status=%d), "
                   "restart %d/%d...\n", status, attempt + 1, MAX_ATTEMPTS);
            usleep(500000);
            continue;
        }
        if (exited) {
            if (status == 0) return 0;          /* clean early exit */
            printf("ff: Firefox exited (status=%d) before paint, restart %d/%d...\n",
                   status, attempt + 1, MAX_ATTEMPTS);
        } else {
            printf("ff: Firefox stalled (no paint in %ds), killing + restart %d/%d...\n",
                   PAINT_TIMEOUT_S, attempt + 1, MAX_ATTEMPTS);
            /* FF appends ".moz_log" to MOZ_LOG_FILE (parent); children get
             * ".child-N.moz_log".  Dump the parent's tail — it owns the toplevel. */
            dump_tail("/tmp/moz.log.moz_log", 16384);   /* where did the parent stall? */
            save_moz_logs(attempt + 1);   /* full parent + child logs onto /disk */
            kill(fpid, 9);
            waitpid(fpid, &status, 0);
        }
        usleep(500000);
    }
    printf("ff: gave up after %d attempts without a painted window.\n", MAX_ATTEMPTS);
    return 1;
}
