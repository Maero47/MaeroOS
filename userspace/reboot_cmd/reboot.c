/* reboot / shutdown / poweroff / halt — calls the Linux reboot(2) syscall.
 * Needs root; run via doas if you are the unprivileged user. */
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/unistd.h"
#include "../include/syscall.h"

#define CMD_POWER_OFF 0x4321FEDC
#define CMD_RESTART   0x01234567

int main(int argc, char *argv[]) {
    const char *me = argv[0];
    const char *base = me;
    for (const char *p = me; *p; p++) if (*p == '/') base = p + 1;

    int power_off = !strcmp(base, "poweroff") || !strcmp(base, "shutdown") ||
                    !strcmp(base, "halt");
    /* `shutdown -r` restarts. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) power_off = 0;
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "-P")) power_off = 1;
    }
    if (geteuid() != 0) {
        printf("%s: must be root (try: doas %s)\n", base, base);
        return 1;
    }
    printf("%s: %s now...\n", base, power_off ? "powering off" : "restarting");
    syscall3(88, 0, 0, power_off ? CMD_POWER_OFF : CMD_RESTART);
    return 1;   /* only reached if the syscall returned */
}
