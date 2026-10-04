/*
 * P76 closing an ALSA PCM while a sibling thread's START is still writing the
 * staged audio, with a signal interrupting the close.
 *
 * Linux: close() drops the descriptor at once, but the file is released only
 * when the running ioctl returns, so the PCM stays busy (a non-blocking open
 * meanwhile is EBUSY) and START finishes normally.
 *
 * MaeroOS before: close waited for the stream mutex interruptibly; the signal
 * made it give up and free the staged buffer while START was still copying
 * it into the DMA ring (use after free), and the device was free to open
 * again before START had returned.
 *
 * A child keeps the output busy through the OSS /dev/dsp (a long blocking
 * write holds the device's writer), so thread B's START, which hands the
 * staged buffer to the same output, blocks.  Thread A closes the fd
 * meanwhile; thread C signals A, then kills the child so START can finish.
 * Then: if a non-blocking reopen succeeded, B's START must already have
 * returned; START must succeed.
 *
 * SKIP without a sound card (/dev/snd/pcmC0D0p); tools/smoke_hda.py runs it.
 */
#define PROBE_NAME "p76_alsa_close_race"
#include "probe.h"
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define DEV "/dev/snd/pcmC0D0p"

static int fd;
static volatile int b_started, b_done, b_rc, a_closing;
static pthread_t ta;
static pid_t hog;

static struct snd_interval *iv(struct snd_pcm_hw_params *p, int n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void set_iv(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    iv(p, n)->min = iv(p, n)->max = v;
    iv(p, n)->integer = 1;
}

static void only(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
    struct snd_mask *m = &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof *m);
    m->bits[bit / 32] = 1u << (bit % 32);
}

static void *start_thread(void *arg)
{
    (void)arg;
    b_started = 1;
    b_rc = ioctl(fd, SNDRV_PCM_IOCTL_START);
    if (b_rc < 0) b_rc = -errno;
    b_done = 1;
    return NULL;
}

static void *signal_thread(void *arg)
{
    (void)arg;
    while (!a_closing) sleep_ms(1);
    sleep_ms(40);
    pthread_kill(ta, SIGUSR1);
    sleep_ms(150);
    kill(hog, SIGKILL);
    return NULL;
}

static void on_usr1(int s) { (void)s; }

int main(void)
{
    probe_watchdog(60);
    fd = open(DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0 && (errno == ENOENT || errno == ENODEV || errno == ENXIO))
        probe_skip("no sound card (%s)", strerror(errno));
    if (fd < 0) probe_fail("open %s: %s", DEV, strerror(errno));

    struct snd_pcm_hw_params hp;
    memset(&hp, 0, sizeof hp);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
        memset(&hp.masks[i], 0xff, sizeof hp.masks[i]);
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
        hp.intervals[i].max = ~0u;
    only(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    only(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    only(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_RATE, 48000);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 1024);
    set_iv(&hp, SNDRV_PCM_HW_PARAM_PERIODS, 46);      /* 47104 frames, 184 KiB */
    hp.rmask = ~0u;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) != 0)
        probe_fail("HW_PARAMS: %s", strerror(errno));
    unsigned frames = iv(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    probe_info("buffer %u frames", frames);
    if (frames * 4 < 160 * 1024)
        probe_skip("buffer of %u frames does not outsize the DMA ring", frames);

    struct snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof sp);
    sp.avail_min = 1024;
    sp.start_threshold = frames * 2;           /* writes only stage */
    sp.stop_threshold = frames;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) != 0)
        probe_fail("SW_PARAMS: %s", strerror(errno));
    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) != 0)
        probe_fail("PREPARE: %s", strerror(errno));

    short *pcm = calloc(frames, 4);           /* silence */
    struct snd_xferi x = { .buf = pcm, .frames = frames };
    if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) != 0 || x.result != (long)frames)
        probe_fail("WRITEI: %s (%ld frames)", strerror(errno), (long)x.result);

    /* The hog: a /dev/dsp writer that never finishes on its own. */
    int pp[2];
    if (pipe(pp) != 0) probe_fail("pipe");
    hog = fork();
    if (hog == 0) {
        close(fd);                    /* the PCM's last close is A's */
        int d = open("/dev/dsp", O_WRONLY);
        char ok = d >= 0 ? 'y' : 'n';
        if (write(pp[1], &ok, 1) != 1 || d < 0) _exit(1);
        static char zero[65536];
        for (;;) if (write(d, zero, sizeof zero) < 0) _exit(0);
    }
    char ok = 'n';
    if (read(pp[0], &ok, 1) != 1 || ok != 'y') {
        kill(hog, SIGKILL);
        waitpid(hog, NULL, 0);
        probe_skip("/dev/dsp not available to keep the output busy");
    }
    sleep_ms(300);                            /* its write holds the output */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;                  /* no SA_RESTART */
    sigaction(SIGUSR1, &sa, NULL);
    ta = pthread_self();
    pthread_t tb, tc;
    pthread_create(&tc, NULL, signal_thread, NULL);
    pthread_create(&tb, NULL, start_thread, NULL);
    while (!b_started) sleep_ms(1);
    sleep_ms(30);                             /* B is inside START */
    int busy_at_close = !b_done;
    a_closing = 1;
    close(fd);
    int done_at_close = b_done;
    int fd2 = open(DEV, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    int e2 = errno;
    int done_at_reopen = b_done;
    pthread_join(tb, NULL);
    pthread_join(tc, NULL);
    waitpid(hog, NULL, 0);
    probe_info("START busy at close: %d, done when close returned: %d, reopen %s "
               "(START done then: %d), START -> %d", busy_at_close, done_at_close,
               fd2 >= 0 ? "ok" : strerror(e2), done_at_reopen, b_rc);
    if (!busy_at_close)
        probe_skip("START finished before the close (no race to test)");
    if (fd2 >= 0 && !done_at_reopen)
        probe_fail("the PCM could be opened again while START was still running "
                   "(released under it)");
    if (fd2 >= 0) close(fd2);
    if (b_rc != 0) probe_fail("START: %d", b_rc);
    probe_pass();
}
