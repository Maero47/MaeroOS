#include "pit.h"
#include "irq.h"
#include "../include/io.h"
#include "../include/registers.h"
#include "../../../kernel/random.h"
#include <stdint.h>

#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43
/* Command: channel 0, lobyte/hibyte, rate generator (mode 2) */
#define PIT_CMD_RATE  0x36

static volatile uint32_t ticks = 0;

static void pit_handler(void *regs) {
    ticks++;
    random_mix_u32(ticks);
    /*
     * Only preempt if the tick interrupted ring 3.  Kernel code (syscalls,
     * exception handlers, kthreads) is never preempted involuntarily — it
     * uses shared non-reentrant state (paging temp maps, lwIP) and always
     * sleeps voluntarily.  CS RPL != 0 → user mode.
     */
    int user_mode = (((registers_t *)regs)->cs & 3) != 0;
    extern void scheduler_tick(int user_mode);
    scheduler_tick(user_mode);
}

void pit_init(uint32_t hz) {
    uint32_t divisor = 1193182 / hz;
    outb(PIT_CMD, PIT_CMD_RATE);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    irq_install_handler(0, (void *)pit_handler);
}

uint32_t pit_ticks(void) { return ticks; }
