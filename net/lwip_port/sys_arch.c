#include "lwip/sys.h"
#include "../../arch/i686/cpu/pit.h"

u32_t sys_now(void) {
    return pit_ticks() * 10U;
}
