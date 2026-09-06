#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../../../kernel/printk.h"

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1

#define LWIP_PLATFORM_DIAG(x) do { printk x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printk("[LWIP] assert: %s\n", x); \
    for (;;) __asm__ volatile("hlt"); \
} while (0)

#define LWIP_RAND() 4

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

