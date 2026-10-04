#pragma once
#include <stdint.h>
#include "../fs/vfs.h"

/*
 * ALSA kernel ABI: card 0 (/dev/snd/controlC0 + /dev/snd/pcmC0D0p, playback
 * only) on top of the HDA or AC'97 PCM-out driver, and card 1 for a USB
 * audio device while one is plugged in.  See drivers/alsa.c.
 */

#define ALSA_MAX_CARDS 2

/* What a card's driver provides.  `write` takes S16LE stereo at `rate`,
 * blocking until it is queued; < 0 once the device is gone.  The volume
 * (0..100) and mute hooks may be NULL: no such mixer element. */
typedef struct {
    const char *name;            /* "HDA", "USB Audio" */
    const char *longname;
    uint32_t rate;
    uint32_t min_buffer_us;      /* smallest buffer it can run (0: any) */
    int      (*write)(const uint8_t *data, uint32_t len);
    uint32_t (*queued)(void);    /* bytes accepted, not yet played */
    void     (*drop)(void);      /* discard them */
    void     (*flush)(void);     /* the stream ends: play a held partial
                                  * packet (may be NULL) */
    int      (*get_volume)(void);
    int      (*set_volume)(int percent);
    int      (*get_mute)(void);
    int      (*set_mute)(int on);
} alsa_out_t;

void        alsa_init(void);          /* after hda_init/ac97_init */
vfs_node_t *alsa_dev_dir(void);       /* /dev/snd, or NULL without a card */
int         alsa_node(const vfs_node_t *n);
/* The whole ioctl, user pointer included (the argument is copied in and out
 * here, not by sys_ioctl); `nonblock` is the descriptor's O_NONBLOCK. */
int         alsa_ioctl(vfs_node_t *n, uint32_t req, void *uarg, int nonblock);
/* A hot-plugged card (index 1) comes and goes. */
void        alsa_card_add(int index, const alsa_out_t *out);
void        alsa_card_remove(int index);
/* Is card `index` there (card 1: a USB audio device plugged in)?  The
 * generation counts its arrivals (a new plug-in: devfs refreshes /dev/dsp1's
 * owner). */
int         alsa_card_present(int index);
uint32_t    alsa_card_generation(int index);
/* The owner, group and mode every sound node starts with (the /dev/snd
 * nodes, /dev/dsp, /dev/dsp1); see drivers/alsa.c. */
void        alsa_node_perms(vfs_node_t *n);
/* /dev/dsp1: raw 48 kHz S16LE stereo straight to card `index`'s driver
 * (bytes taken, or -errno: -ENODEV unplugged, -EINVAL at another rate). */
int         alsa_raw_write(int index, const uint8_t *data, uint32_t len);
/* Card `index`'s Master Playback Volume: set it when `set` >= 0; returns
 * the value (0..100) or -errno. */
int         alsa_master_volume(int index, int set);
