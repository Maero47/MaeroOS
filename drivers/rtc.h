#pragma once
#include <stdint.h>

/* Sample the CMOS RTC once; call before interrupts matter (boot). */
void rtc_init(void);

/* Unix epoch seconds at boot (0 if rtc_init was never called). */
uint32_t rtc_boot_epoch(void);
