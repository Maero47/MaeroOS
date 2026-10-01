#pragma once
#include <stdint.h>

/* Intel High Definition Audio PCM-out driver (QEMU -device intel-hda /
 * ich9-intel-hda + hda-output/hda-duplex).  Fixed 48 kHz S16LE stereo, the
 * same stream /dev/dsp carries for AC'97. */

void hda_init(void);             /* probe PCI; safe to call when absent */
void hda_start_thread(void);     /* call after proc_init (spawns khdad) */
int  hda_present(void);
int  hda_write(const uint8_t *data, uint32_t len);   /* blocking */
int  hda_set_volume(int percent);                    /* 0..100; <0 on error */
int  hda_get_volume(void);
