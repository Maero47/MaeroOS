#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "alsactl.h"

/*
 * mixer — the Master volume of an ALSA card (drivers/alsa.c's mixer
 * elements), like `amixer sget/sset Master`:
 *   mixer [-c card]               show Master Playback Volume and Switch
 *   mixer [-c card] <0-100>       set the volume (percent)
 *   mixer [-c card] mute|unmute   set the switch
 */
int main(int argc, char **argv) {
    int card = 0, fd, vol, sw;
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        card = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    }
    fd = alsactl_open(card);
    if (fd < 0) {
        printf("mixer: no card %d\n", card);
        return 1;
    }
    if (argc > 1) {
        int r;
        if (strcmp(argv[1], "mute") == 0 || strcmp(argv[1], "unmute") == 0)
            r = alsactl_elem(fd, "Master Playback Switch",
                             strcmp(argv[1], "unmute") == 0);
        else
            r = alsactl_elem(fd, "Master Playback Volume", atoi(argv[1]));
        if (r < 0) {
            printf("mixer: card %d: cannot set %s\n", card, argv[1]);
            return 1;
        }
    }
    vol = alsactl_elem(fd, "Master Playback Volume", -1);
    sw = alsactl_elem(fd, "Master Playback Switch", -1);
    close(fd);
    if (vol < 0 && sw < 0) {
        printf("mixer: card %d has no Master control\n", card);
        return 1;
    }
    printf("card %d Master:", card);
    if (vol >= 0) printf(" %d%%", vol);
    if (sw >= 0) printf(" [%s]", sw ? "on" : "off");
    printf("\n");
    return 0;
}
