#pragma once
#include <stdint.h>
#include "../fs/vfs.h"

/*
 * ALSA kernel ABI (card 0: /dev/snd/controlC0 + /dev/snd/pcmC0D0p, playback
 * only) on top of the HDA or AC'97 PCM-out driver.  See drivers/alsa.c.
 */

void        alsa_init(void);          /* after hda_init/ac97_init */
vfs_node_t *alsa_dev_dir(void);       /* /dev/snd, or NULL without a card */
int         alsa_node(const vfs_node_t *n);
/* The whole ioctl, user pointer included (the argument is copied in and out
 * here, not by sys_ioctl); `nonblock` is the descriptor's O_NONBLOCK. */
int         alsa_ioctl(vfs_node_t *n, uint32_t req, void *uarg, int nonblock);
