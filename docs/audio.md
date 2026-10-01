# Sound: /dev/dsp and the ALSA kernel ABI

## Using it

```sh
tone 440 1000                        # MaeroOS's own player, through /dev/dsp
toybox chroot /disk/alpine aplay -l  # Alpine's alsa-utils: "card 0: MaeroOS [MaeroOS HDA]"
toybox chroot /disk/alpine aplay /tmp/some.wav
toybox chroot /disk/alpine aplay -D hw:0 /tmp/some.wav    # no alsa-lib plug layer
```

QEMU needs a sound card with an audio backend, e.g.
`-device intel-hda -device hda-duplex,audiodev=a -audiodev pa,id=a` (or
`-device AC97,audiodev=a`); `-audiodev wav,id=a,path=out.wav` records what the
card plays.

## Pieces

| File | Role |
|---|---|
| `drivers/hda.c` | Intel HDA: codec walk, one output stream, 48 kHz S16LE stereo from a 256 KiB byte ring through a 128 KiB cyclic DMA buffer; `hda_queued()` (bytes not yet played) and `hda_drop()` for ALSA |
| `drivers/ac97.c` | AC'97: the same ring and the same cyclic scheme over its 32 x 4 KiB buffer list (absolute play/write positions from CIV/PICB, LVI kept behind CIV); `ac97_queued()`, `ac97_drop()` |
| `fs/devfs.c` | `/dev/dsp` (raw 48 kHz S16LE stereo writes, OSS mixer volume on HDA) and `/dev/snd` |
| `drivers/alsa.c` | `/dev/snd/controlC0` and `/dev/snd/pcmC0D0p`: the ALSA control and PCM playback ioctls, format conversion and resampling |
| `proc/syscall.c` | `ioctl` on a `/dev/snd` node goes to `alsa_ioctl()` (with the descriptor's `O_NONBLOCK`); `mmap` of one fails with `ENXIO` |

## The ALSA ABI subset

Applications built for Linux talk to `libasound` (alsa-lib), whose `hw`
plugin talks to the kernel through `/dev/snd/*` ioctls. `drivers/alsa.c`
provides what that plugin needs for playback. Structure layouts are the i386
ones from the ALSA UAPI (PCM protocol 2.0.15, control 2.0.9), written out
from the published ABI rather than copied. The ioctl number carries each
structure's size, which is how the two `time_t` flavours are told apart:
musl (Alpine) has a 64-bit `time_t` and uses the 128-byte `STATUS` and
136-byte `SYNC_PTR`; Debian i386 glibc (the `libasound` next to Firefox)
has a 32-bit one and uses 108 and 132 bytes. Both are answered.

Control device: `PVERSION`, `CARD_INFO` (driver name `MaeroOS`, which has
no `cards/*.conf` in alsa-lib, so the `default` PCM is `plug` over `hw`),
`PCM_NEXT_DEVICE`, `PCM_INFO`, `PCM_PREFER_SUBDEVICE`, an empty
`ELEM_LIST`, `SUBSCRIBE_EVENTS`, and the "no such device" answers for
hwdep, rawmidi and UMP.

PCM device: `PVERSION`, `INFO`, `TSTAMP`/`TTSTAMP`/`USER_PVERSION`,
`HW_REFINE`, `HW_PARAMS`, `HW_FREE`, `SW_PARAMS`, `STATUS`, `STATUS_EXT`,
`DELAY`, `HWSYNC`, `SYNC_PTR`, `PREPARE`, `RESET`, `START`, `DROP`, `DRAIN`
(blocking and non-blocking), `XRUN`, `WRITEI_FRAMES`, and `poll()` for
`POLLOUT` once `avail >= avail_min`.

- **Parameters.** Access `RW_INTERLEAVED`; formats S8, U8, S16_LE, S24_LE,
  S32_LE, FLOAT_LE; 1 or 2 channels; 8 to 192 kHz (48 kHz only with the
  `NORESAMPLE` flag); 2 to 1024 periods; a buffer of at most 1 s.
  `HW_REFINE` intersects the request with those limits and then applies the
  relations between the parameters (frame bits = sample bits x channels,
  period bytes = period size x frame bits / 8, period time = period size x
  10^6 / rate, buffer = period size x periods, and the rest of that family)
  in interval arithmetic until nothing changes, the way a Linux driver's
  constraint rules do; alsa-lib's `snd_pcm_hw_params_set_*_near` and its
  plug layer depend on that.
- **Data path.** The hardware stream is fixed at 48 kHz S16LE stereo.
  `WRITEI_FRAMES` decodes each frame to 16-bit stereo (float through its bit
  pattern, as the kernel uses no FPU), resamples to 48 kHz by linear
  interpolation and hands the result to the HDA or AC'97 ring. Before the
  stream starts (`start_threshold`, or `START`), the converted PCM waits in
  a staging buffer.
- **Pointers.** `appl_ptr` is what the application wrote; `hw_ptr` is
  derived from what the driver still has queued (ring plus DMA ahead of the
  engine), converted back to the application's rate, so `delay` is the real
  latency and `avail = buffer_size - delay`. The ring holds 1.36 s, more
  than the largest buffer, so a write that fits in `avail` never blocks in
  the driver. Running dry while `avail >= stop_threshold` is an XRUN
  (`-EPIPE`), as on Linux.
- **Not provided.** mmap of the status/control pages (alsa-lib falls back
  to `SYNC_PTR`, as on any kernel without them) and of the sample buffer (no
  `MMAP_*` access, so `dmix` cannot run; the `default` device does not use
  it here), capture, pause and resume, linked streams, mixer controls (no
  `amixer` elements; `/dev/dsp`'s OSS volume ioctl still works on HDA),
  timers (`/dev/snd/timer`). The PCM has one substream: a second open fails
  (`ENOENT` here, `EBUSY` on Linux).

## Tests

`make smoke-audio` (opt-in: it needs `disk-alpine.img` with `alsa-utils`,
`ports/alpine/prepare.py`) writes three WAVs into a copy of the Alpine image
and plays them with Alpine's unmodified `aplay` in the chroot, then plays a
fourth tone with `tone` through `/dev/dsp`; QEMU's wav capture must hold
each tone, in order, at its frequency (DFT peak and zero crossings), for
about its length and without dropouts:

| Tone | File | Device |
|---|---|---|
| 440 Hz, 1.5 s | 44.1 kHz mono S16_LE | `default` (alsa-lib `plug` over `hw`) |
| 880 Hz, 1 s | 22.05 kHz stereo U8 | `hw:0` |
| 1320 Hz, 1 s | 48 kHz stereo FLOAT_LE | `hw:0` |
| 2000 Hz, 0.5 s | `tone 2000 500` | `/dev/dsp` |

`python3 tools/smoke_audio.py --ac97` runs the same on an AC'97 card.
`make smoke-hda` (in `make check`) still covers `/dev/dsp` on HDA.

Firefox: `python3 tools/smoke_firefox.py --audio` (see below).
