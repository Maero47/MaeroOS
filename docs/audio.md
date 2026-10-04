# Sound: /dev/dsp and the ALSA kernel ABI

## Using it

```sh
tone 440 1000                        # MaeroOS's own player, through /dev/dsp
toybox chroot /disk/alpine aplay -l  # Alpine's alsa-utils: "card 0: MaeroOS [MaeroOS HDA]"
toybox chroot /disk/alpine aplay /tmp/some.wav
toybox chroot /disk/alpine aplay -D hw:0 /tmp/some.wav    # no alsa-lib plug layer
mixer -c 0 40                        # card 0's Master to 40 % (also: mute, unmute)
toybox chroot /disk/alpine amixer -c 0 sset Master 50%
tone -c 1 440 1000                   # a USB audio device: ALSA card 1
tone -d /dev/dsp1 440 1000           # the same card's raw 48 kHz node
toybox chroot /disk/alpine aplay -D hw:1 /tmp/some.wav
```

The desktop's Volume Up / Down / Mute keys set the Master volume and switch
of every card (`[desktop] volume 55 (mixer: card0 55%)` in its trace).

QEMU needs a sound card with an audio backend, e.g.
`-device intel-hda -device hda-duplex,audiodev=a -audiodev pa,id=a` (or
`-device AC97,audiodev=a`); `-audiodev wav,id=a,path=out.wav` records what the
card plays.  A USB audio device: `-device qemu-xhci -device
usb-audio,audiodev=a`.

## Pieces

| File | Role |
|---|---|
| `drivers/hda.c` | Intel HDA: codec walk, one output stream, 48 kHz S16LE stereo from a 256 KiB byte ring through a 128 KiB cyclic DMA buffer; `hda_queued()` (bytes not yet played) and `hda_drop()` for ALSA |
| `drivers/ac97.c` | AC'97: the same ring and the same cyclic scheme over its 32 x 4 KiB buffer list (absolute play/write positions from CIV/PICB, LVI kept behind CIV); `ac97_queued()`, `ac97_drop()` |
| `fs/devfs.c` | `/dev/dsp` (raw 48 kHz S16LE stereo writes, OSS mixer volume on HDA), `/dev/dsp1` (the same on card 1 while a 48 kHz USB card is plugged in; its OSS volume is the card's Master) and `/dev/snd` |
| `drivers/alsa.c` | `/dev/snd/controlC<n>` and `/dev/snd/pcmC<n>D0p` for card 0 (HDA/AC'97) and card 1 (USB, while plugged in): the ALSA control and PCM playback ioctls, the mixer elements, format conversion and resampling |
| `drivers/usb/usb_audio.c` | USB Audio Class 1/2 playback on xHCI isochronous TDs (`drivers/usb/xhci.c`): the descriptors, the alternate setting and rate, the Feature Unit's volume and mute, a 256 KiB FIFO feeding one TD per service interval |
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
`PCM_NEXT_DEVICE`, `PCM_INFO`, `PCM_PREFER_SUBDEVICE`, `ELEM_LIST`,
`ELEM_INFO`, `ELEM_READ`, `ELEM_WRITE`, `ELEM_LOCK`/`UNLOCK`,
`SUBSCRIBE_EVENTS`, and the "no such device" answers for hwdep, rawmidi and
UMP.

Mixer elements: "Master Playback Volume" (integer 0..100, one value) and
"Master Playback Switch" (boolean, 1 = sound on), which alsa-lib's simple
mixer shows as `Master`.  On HDA they are the output amplifiers' gain and
mute (the same gain `/dev/dsp`'s OSS mixer sets); on a USB card the Feature
Unit's master (or per-channel) volume, the percentage spread over the top
48 dB of its range, and its mute.  An AC'97 card has none.  An element is
found by numid or by interface + name + index, as alsa-lib looks them up.
No dB TLVs and no change events (`read()` on the control device returns
nothing).

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
- **Data path.** The hardware stream is fixed at 48 kHz S16LE stereo (a
  USB card: 16-bit stereo at the rate the device was set to, 48 or
  44.1 kHz).  `WRITEI_FRAMES` decodes each frame to 16-bit stereo (float
  through its bit pattern, as the kernel uses no FPU), resamples to the
  card's rate by linear interpolation and hands the result to the HDA or
  AC'97 ring or the USB driver's FIFO. Before the
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
  it here), capture, pause and resume, linked streams, mixer elements beyond
  Master, timers (`/dev/snd/timer`). The PCM has one substream: a second open fails
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
`make AUDIO_POS_START=0xFFFE0000U` builds a kernel whose sound streams start
128 KiB before the drivers' 32-bit position wrap. With it, the three `aplay`
tones each cross the wrap during smoke-audio. The drivers compare positions
only through signed differences (`POS_LT`); with plain comparisons the same run
underruns at the wrap and hangs.

`tools/test_alsa_resample.c` runs the resampler (`drivers/alsa_resample.h`)
on the host:

    cc -O2 -o build/test_alsa_resample tools/test_alsa_resample.c -lm && build/test_alsa_resample

It feeds a full-scale 44.1 kHz square wave and a sine. Every output frame must
be within 1 LSB of the exact interpolation, and 48000/44100 as many frames must
come out.
`make smoke-hda` (in `make check`) still covers `/dev/dsp` on HDA, and runs
probe p77 on the mixer elements.  smoke-audio also drives Alpine's `amixer
-c 0` and plays `aplay -D hw:1` on a `usb-audio` device.

## USB audio (card 1)

`drivers/usb/usb_audio.c` takes a USB Audio Class 1 (or 2) device with a
PCM playback interface; one at a time, as card 1.  It prefers 16-bit stereo
at 48 kHz (then other widths, mono, 44.1 kHz), selects that alternate
setting, sets the rate (UAC1: SET_CUR on the endpoint when it has the
control; UAC2: on the clock source), and finds the Feature Unit between the
stream's input terminal and an output terminal for the mixer elements.
`write` converts ALSA's 16-bit stereo to the device's format into a 256 KiB
FIFO; TDs of one service interval each (the rate x interval, remainder
carried: 44.1 kHz on 1 ms sends 44 frames nine times and 45 the tenth) are
copied into 512-byte DMA slots and queued on the endpoint's isochronous
ring with Start Isoch ASAP, at most 128 at a time.  Each completion, handled
on kusbd's interrupt, frees its slot, queues more and wakes `poll()`.  The
stream stops by running dry (Ring Underrun) and starts again with the next
write; DRAIN pads the last partial TD with silence.  ALSA's buffer is at
least 20 ms on this card, so it always holds a whole TD.  Adaptive and
synchronous endpoints need nothing more; an asynchronous one is played at
the nominal rate without reading its feedback endpoint.  Unplugged, the
card leaves `/dev/snd` and an open PCM fails with `ENODEV`
(`SNDRV_PCM_STATE_DISCONNECTED`) until it is closed.

Permissions: every sound node starts from `alsa_node_perms()` in
`drivers/alsa.c`.  Card 1's `/dev/snd` nodes and `/dev/dsp1` have none of
their own: they share card 0's and `/dev/dsp`'s through `perm_of`
(`fs/vfs.h`), which access checks, `stat` and `chmod`/`chown` follow, so
whatever init (or anyone) does to card 0's owner or mode applies to the USB
card at once, plugged in or not.  A tree with root:audio 0660 sound nodes
sets that in `alsa_node_perms()`.

`make smoke-usbaudio` (in `make check`) plays tones on QEMU's `usb-audio`
with `tone -c 1`, checks them in the wav capture, sets the mixer of both
cards with `mixer`, and unplugs and replugs the device in the middle of a
tone.

Firefox: `python3 tools/smoke_firefox.py --audio` (see below).

## Firefox

Firefox 115 ESR reaches the sound card. Once this work was merged with the
rest of the tree (PAE paging, the compositor's damage tracking and frame pacing,
the `rep movs`/`stos` libc string functions, among others), three runs of
`smoke_firefox.py --audio` passed: the 3 s, 440 Hz clip was captured for
2.99-3.00 s with 5 silent 10 ms blocks inside (2 after the vruntime
scheduler, one run). On the audio branch alone the
same test showed the choppy playback described below. Which later change made
the difference has not been isolated, so the analysis is kept as it was
measured.

**How the path works.** Mozilla's Linux builds do not compile cubeb's ALSA
backend. `libxul.so` has no `snd_pcm_open_lconf`, no `snd_config_*` and no
`cubeb_alsa` strings, and its `libasound.so.2` dependency comes from WebMIDI's
Rust `alsa` crate. The only audio backends are cubeb's two PulseAudio ones,
so `media.cubeb.backend=alsa` falls through to them. Without a
`libpulse.so.0`, `cubeb_init` fails and `<audio>` ends in
`MEDIA_ERR_DECODE` ("Failed to decode media").
`ports/firefox/fetch-runtime.sh` therefore installs Debian's **apulse** (MIT)
as `testfiles/firefox/apulse/`. apulse is a `libpulse.so.0` that plays
through alsa-lib. The script also installs alsa-lib's configuration tree as
`testfiles/firefox/alsa/`. The `ff` launcher puts the apulse directory on
`LD_LIBRARY_PATH` and sets `ALSA_CONFIG_DIR=/disk/firefox/alsa`. The path is:

    <audio> -> cubeb pulse-rust -> apulse -> Debian libasound (glibc, 32-bit time_t)
            -> /dev/snd/pcmC0D0p -> drivers/alsa.c -> HDA

Debian's own `aplay` from the same glibc and libasound plays a clean 440 Hz
tone through this ABI. That run used the 108-byte `STATUS` and 132-byte
`SYNC_PTR` layouts.

**What `smoke_firefox.py --audio` showed on the audio branch.** The page opens from
`file:///disk/audio.html` with no network involved. The clip's `play`,
`playing` and `ended` events fire, and `ended` comes at `currentTime` 3. The
kernel takes about 44,100 frames/s from apulse, which is real time. For
roughly the first 1.3 s the capture holds a clean 440 Hz tone. After that it
alternates tone and silence in 10 ms blocks, and the 3 s clip takes 10 to
19 s to finish.

**The blocker on that branch was inside Firefox, not in the kernel.** Firefox
logs `W/AudioStream ... lost N frames` (MOZ_LOG `AudioStream:2`) on almost
every cubeb callback. In each callback its AudioSink has only about one
decoded packet (about 1000 frames) ready, whatever the period: with 25 ms
periods (1104 frames) it loses about 900 frames, and with 125 ms periods
(`media.cubeb_latency_playback_ms=500`) it loses about 4,400 of 5,514. So
decoded audio reaches the sink at about a third of real time, and apulse
fills the gaps with silence: 25k-33k of every 45k frames that arrive in the
kernel are zero. Raising `media.audio.audiosink.threshold_ms` (200, 500)
and running with `-smp 2` did not change this. The rate is the same for a
plain WAV, so the decode work itself is not the limit. Each packet makes a
round trip from MediaDecoderStateMachine to the decoder task queue and back
to the AudioSink, and that round trip was slow on the scheduler of the time
(a 20 ms quantum; wakeups from interrupts did not preempt). The vruntime
scheduler that replaced it (4 ms slices, wakeup preemption, a resched IPI)
leaves 2 silent 10 ms blocks inside the same 3 s clip, against 5 just before
it on the integrated tree.

Two earlier failures were also fixed on the way. The `/dev/snd` nodes were
0660 root and Firefox runs as `user`; they are now 0666 like `/dev/dsp`.
Tracing every ALSA ioctl to the serial console (`ALSA_TRACE` in
`drivers/alsa.c`) slowed playback enough to cause XRUNs on its own, so the
trace is off by default.
