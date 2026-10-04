/*
 * P77 the ALSA control interface's mixer elements, as alsa-lib's simple
 * mixer (amixer) uses them, on card 0.
 *
 * Linux: SNDRV_CTL_IOCTL_ELEM_LIST reports the element count and fills the
 * caller's id array up to `space`; ELEM_INFO, ELEM_READ and ELEM_WRITE find
 * an element by numid or, with numid 0, by interface + name + index; a
 * "Master Playback Volume" integer and a "Master Playback Switch" boolean
 * make amixer's "Master" control.  A name that does not exist is ENOENT.
 *
 * MaeroOS before: ELEM_LIST said 0 elements and every other element ioctl
 * failed with ENOENT, so there was no volume control through ALSA.
 *
 * Checked: the list (count, then the ids with a space of count), both
 * elements' info (integer with min < max, boolean 0..1, readable and
 * writable), a volume write by name read back by numid, the switch off and
 * on, the unknown name; on MaeroOS also that the OSS mixer of /dev/dsp sees
 * the same volume (one HDA amplifier behind both).  The original values are
 * put back.
 *
 * SKIP without a card with those elements; tools/smoke_hda.py runs it.
 */
#define PROBE_NAME "p77_alsa_mixer"
#include "probe.h"
#include <sound/asound.h>
#include <sys/ioctl.h>

#define DEV "/dev/snd/controlC0"
#define VOL "Master Playback Volume"
#define SW  "Master Playback Switch"
#define SOUND_MIXER_READ_VOLUME 0x80044D00U

static int fd;

static void by_name(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof *id);
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)id->name, name, sizeof id->name - 1);
}

static long rd(unsigned numid, const char *name)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    if (numid) v.id.numid = numid;
    else by_name(&v.id, name);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        probe_fail("ELEM_READ %s (numid %u): %s", name, numid, strerror(errno));
    return v.value.integer.value[0];
}

static void wr(const char *name, long val, unsigned count)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    by_name(&v.id, name);
    for (unsigned i = 0; i < count && i < 128; i++)
        v.value.integer.value[i] = val;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0)
        probe_fail("ELEM_WRITE %s = %ld: %s", name, val, strerror(errno));
}

int main(void)
{
    struct snd_ctl_card_info ci;
    struct snd_ctl_elem_list l;
    struct snd_ctl_elem_id ids[64];
    struct snd_ctl_elem_info vi, si;
    unsigned vol_id = 0, sw_id = 0;

    probe_watchdog(60);
    fd = open(DEV, O_RDWR);
    if (fd < 0) probe_skip("%s: %s", DEV, strerror(errno));
    memset(&ci, 0, sizeof ci);
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) < 0)
        probe_fail("CARD_INFO: %s", strerror(errno));
    probe_info("card %d: %s (%s)", ci.card, (char *)ci.name, (char *)ci.mixername);

    memset(&l, 0, sizeof l);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0)
        probe_fail("ELEM_LIST (count): %s", strerror(errno));
    if (l.count == 0) probe_skip("card 0 has no mixer elements");
    if (l.used != 0) probe_fail("ELEM_LIST with space 0 used %u", l.used);
    memset(ids, 0, sizeof ids);
    l.space = l.count < 64 ? l.count : 64;
    l.pids = ids;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &l) < 0)
        probe_fail("ELEM_LIST (ids): %s", strerror(errno));
    if (l.used != l.space) probe_fail("ELEM_LIST used %u of space %u", l.used, l.space);
    for (unsigned i = 0; i < l.used; i++) {
        if (ids[i].iface != SNDRV_CTL_ELEM_IFACE_MIXER || ids[i].index) continue;
        if (!strcmp((char *)ids[i].name, VOL)) vol_id = ids[i].numid;
        if (!strcmp((char *)ids[i].name, SW)) sw_id = ids[i].numid;
    }
    probe_info("%u elements; volume numid %u, switch numid %u", l.count, vol_id, sw_id);
    if (!vol_id || !sw_id) probe_skip("no Master volume and switch on card 0");

    memset(&vi, 0, sizeof vi);
    vi.id.numid = vol_id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &vi) < 0)
        probe_fail("ELEM_INFO volume: %s", strerror(errno));
    if (vi.type != SNDRV_CTL_ELEM_TYPE_INTEGER || vi.count < 1 ||
        vi.value.integer.min >= vi.value.integer.max ||
        (vi.access & SNDRV_CTL_ELEM_ACCESS_READWRITE) != SNDRV_CTL_ELEM_ACCESS_READWRITE ||
        strcmp((char *)vi.id.name, VOL))
        probe_fail("volume info: type %d count %u range %ld..%ld access %x name '%s'",
                   vi.type, vi.count, vi.value.integer.min, vi.value.integer.max,
                   vi.access, (char *)vi.id.name);
    by_name(&si.id, SW);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &si) < 0)
        probe_fail("ELEM_INFO switch by name: %s", strerror(errno));
    if (si.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN || si.id.numid != sw_id ||
        si.value.integer.max != 1)
        probe_fail("switch info: type %d numid %u max %ld", si.type, si.id.numid,
                   si.value.integer.max);
    probe_info("volume %ld..%ld step %ld, %u value(s)", vi.value.integer.min,
               vi.value.integer.max, vi.value.integer.step, vi.count);

    long v0 = rd(vol_id, VOL), s0 = rd(sw_id, SW);
    long lo = vi.value.integer.min, hi = vi.value.integer.max;
    long mid = lo + (hi - lo) * 37 / 100;
    if (mid == v0) mid = lo + (hi - lo) * 61 / 100;
    wr(VOL, mid, vi.count);
    if (rd(vol_id, VOL) != mid || rd(0, VOL) != mid)
        probe_fail("volume %ld written, %ld read back", mid, rd(vol_id, VOL));
    {
        /* MaeroOS: the OSS mixer of /dev/dsp is the same HDA amplifier */
        int dsp = open("/dev/dsp", O_WRONLY | O_NONBLOCK), oss = -1;
        if (dsp >= 0 && hi == 100 &&
            ioctl(dsp, SOUND_MIXER_READ_VOLUME, &oss) == 0 &&
            (oss & 0xFF) != mid)
            probe_fail("OSS volume %d, ALSA %ld", oss & 0xFF, mid);
        if (dsp >= 0) close(dsp);
        probe_info("OSS mixer reads %d", oss & 0xFF);
    }
    wr(SW, 0, si.count);
    if (rd(sw_id, SW) != 0) probe_fail("switch still on after writing 0");
    wr(SW, 1, si.count);
    if (rd(sw_id, SW) != 1) probe_fail("switch still off after writing 1");

    {
        struct snd_ctl_elem_value v;
        memset(&v, 0, sizeof v);
        by_name(&v.id, "No Such Playback Volume");
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0 || errno != ENOENT)
            probe_fail("unknown element: %s instead of ENOENT", strerror(errno));
    }

    wr(VOL, v0, vi.count);
    wr(SW, s0, si.count);
    if (rd(vol_id, VOL) != v0 || rd(sw_id, SW) != s0)
        probe_fail("original values not restored");
    probe_pass();
}
