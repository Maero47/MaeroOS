#include "rtc.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"

/*
 * CMOS RTC reader — sampled once at boot to anchor wall-clock time.
 * gettimeofday then returns boot_epoch + PIT uptime.
 */

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

static uint32_t boot_epoch;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static long days_from_civil(int y, unsigned m, unsigned d) {
    long era;
    unsigned yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

void rtc_init(void) {
    uint8_t sec, min, hour, day, mon, year, century, regb;
    uint8_t s2, m2, h2, d2, mo2, y2;

    /* Read twice until stable (avoids racing the RTC update cycle). */
    do {
        while (update_in_progress()) {}
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
        day  = cmos_read(0x07);
        mon  = cmos_read(0x08);
        year = cmos_read(0x09);
        while (update_in_progress()) {}
        s2  = cmos_read(0x00);
        m2  = cmos_read(0x02);
        h2  = cmos_read(0x04);
        d2  = cmos_read(0x07);
        mo2 = cmos_read(0x08);
        y2  = cmos_read(0x09);
    } while (sec != s2 || min != m2 || hour != h2 ||
             day != d2 || mon != mo2 || year != y2);

    century = cmos_read(0x32);   /* QEMU provides the century register */
    regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {        /* BCD mode */
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = (uint8_t)(bcd_to_bin(hour & 0x7F) | (hour & 0x80));
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }
    if (!(regb & 0x02)) {        /* 12-hour mode */
        int pm = hour & 0x80;
        hour &= 0x7F;
        if (pm && hour < 12) hour = (uint8_t)(hour + 12);
        if (!pm && hour == 12) hour = 0;
    }

    {
        int full_year = (century >= 19 && century <= 30)
                        ? century * 100 + year : 2000 + year;
        long days = days_from_civil(full_year, mon, day);
        boot_epoch = (uint32_t)(days * 86400L + hour * 3600 + min * 60 + sec);
        printk("[RTC] %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %u)\n",
               full_year, mon, day, hour, min, sec, (unsigned)boot_epoch);
    }
}

uint32_t rtc_boot_epoch(void) {
    return boot_epoch;
}
