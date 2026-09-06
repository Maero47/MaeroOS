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
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

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
    "MOZ_LOG=sync,timestamp,Widget:5,Compositor:5,WebRender:5,AppShell:5,DocShell:4",
    /* Route Firefox's own MOZ_LOG to a file (stderr→/dev/tty routing doesn't reach
     * serial reliably).  The watchdog dumps this file's TAIL to stdout on a stall,
     * so we can see WHERE the parent stalls (e.g. never creating the main XUL
     * browser window).  FF appends ".child-PID" for child processes; the base name
     * is the PARENT (the one that creates the toplevel window we care about). */
    "MOZ_LOG_FILE=/tmp/moz.log",
    (char *)0
};

static void copyfile(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return; }
    char buf[4096];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);
    close(in);
    close(out);
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
    printf("Starting maeroX X server in a desktop slot...\n");
    int pid = fork();
    if (pid == 0) {
        char *a[] = { "/disk/maerox", "3", "-D", (char *)0 };
        execve(a[0], a, ff_envp);
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

    /* Watchdog restart.  Firefox's startup hits a per-launch intermittent
     * stall (a glibc-2.36 condvar lost-wakeup, BZ#25847, that can't be fixed
     * here — no glibc toolchain) where it connects to X but never paints.  It
     * also sometimes crashes early.  So, like a session manager: launch, wait
     * up to PAINT_TIMEOUT for maeroX to drop /tmp/ff_painted (first PutImage);
     * if it paints, keep running; if it crashes or stalls, kill the tree and
     * relaunch.  One of the attempts gets past the race. */
    const int MAX_ATTEMPTS = 20;
    const int PAINT_TIMEOUT_DS = 400;   /* 40 s in 100 ms units.  The startup
                                         * stall is binary per-attempt: an attempt
                                         * that is going to paint does so quickly
                                         * (<35 s); one that hits the intermittent
                                         * glibc-condvar stall never paints.  So a
                                         * SHORT timeout + many watchdog retries
                                         * maximizes the chance of hitting a good
                                         * attempt (the run that painted did so on
                                         * attempt 2 within 35 s). */
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
            execve(a[0], a, ff_envp);
            _exit(127);
        }
        if (fpid < 0) return 1;

        int status = 0, painted = 0, exited = 0;
        for (int t = 0; t < PAINT_TIMEOUT_DS; t++) {
            if (waitpid(fpid, &status, 1 /* WNOHANG */) == fpid) { exited = 1; break; }
            if (access("/tmp/ff_painted", F_OK) == 0) { painted = 1; break; }
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
                   PAINT_TIMEOUT_DS / 10, attempt + 1, MAX_ATTEMPTS);
            /* FF appends ".moz_log" to MOZ_LOG_FILE (parent); children get
             * ".child-N.moz_log".  Dump the parent's tail — it owns the toplevel. */
            dump_tail("/tmp/moz.log.moz_log", 16384);   /* where did the parent stall? */
            kill(fpid, 9);
            waitpid(fpid, &status, 0);
        }
        usleep(500000);
    }
    printf("ff: gave up after %d attempts without a painted window.\n", MAX_ATTEMPTS);
    return 1;
}
