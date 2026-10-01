/*
 * The curated Alpine Linux X11 applications the desktop offers (docs/alpinex.md).
 * Shared by the set-uid helper /disk/xapp, which installs, removes and runs
 * them, and the "Linux Apps" window, which lists them.  The helper accepts
 * no package or command that is not in this table.
 */
#pragma once

typedef struct {
    const char *name;        /* id: launcher entry, /disk/apps/<name>/ */
    const char *title;       /* shown in Linux Apps */
    const char *packages;    /* apk add, space separated */
    const char *binary;      /* in the Alpine root: "installed" test */
    const char *command;     /* argv, space separated, run in the chroot */
    const char *about;
} xapp_t;

static const xapp_t xapps[] = {
    { "xterm", "XTerm", "xterm", "/usr/bin/xterm",
      "xterm", "The standard X terminal emulator" },
    { "xeyes", "xeyes", "xeyes", "/usr/bin/xeyes",
      "xeyes", "A pair of eyes that follow the pointer" },
    { "xclock", "xclock", "xclock", "/usr/bin/xclock",
      "xclock", "Analog clock (X Toolkit)" },
    { "mousepad", "Mousepad", "mousepad font-dejavu hicolor-icon-theme", "/usr/bin/mousepad",
      "mousepad", "Text editor (GTK 3)" },
    { "galculator", "Galculator", "galculator font-dejavu hicolor-icon-theme", "/usr/bin/galculator",
      "galculator", "Scientific calculator (GTK 3)" },
    { "ristretto", "Ristretto", "ristretto font-dejavu adwaita-icon-theme", "/usr/bin/ristretto",
      "ristretto /usr/share/icons/hicolor", "Image viewer (GTK 3)" },
    { "feh", "feh", "feh", "/usr/bin/feh",
      "feh -g 640x480 -. /usr/share/feh/images", "Lightweight image viewer" },
    { "mpv", "mpv", "mpv font-dejavu alsa-utils", "/usr/bin/mpv",
      "mpv --vo=x11 --ao=alsa --force-window=yes --keep-open=yes --idle=yes "
      "/usr/share/sounds/alsa/Front_Center.wav",
      "Media player (X11 output, ALSA sound); plays a sample" },
    { "gimp", "GIMP", "gimp font-dejavu adwaita-icon-theme", "/usr/bin/gimp",
      "gimp --no-splash", "Image editor (large: about 230 MB)" },
};
#define N_XAPPS ((int)(sizeof(xapps) / sizeof(xapps[0])))

#define ALPINE_ROOT "/disk/alpine"
#define XAPP_HELPER "/disk/xapp"

static inline const xapp_t *xapp_find(const char *name) {
    for (int i = 0; i < N_XAPPS; i++) {
        const char *a = xapps[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return &xapps[i];
    }
    return 0;
}
