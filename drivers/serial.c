#include "serial.h"
#include <io.h>
#include <stdint.h>
#include "../arch/i686/cpu/apic.h"
#include "../arch/i686/cpu/percpu.h"
#include "../arch/i686/cpu/spinlock.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"

#define COM1 0x3F8
#define COM1_IRQ 4

/* COM1 register offsets */
#define UART_DATA   0   /* Data register (DLAB=0) / Divisor LSB (DLAB=1) */
#define UART_IER    1   /* Interrupt enable (DLAB=0) / Divisor MSB (DLAB=1) */
#define UART_IIR    2   /* Interrupt identification (read) */
#define UART_FCR    2   /* FIFO control (write) */
#define UART_LCR    3   /* Line control */
#define UART_MCR    4   /* Modem control */
#define UART_LSR    5   /* Line status */

#define UART_LCR_DLAB   0x80    /* Divisor latch access bit */
#define UART_LCR_8N1    0x03    /* 8 data bits, no parity, 1 stop bit */
#define UART_LSR_THRE   0x20    /* Transmit-hold-register empty */
#define UART_LSR_DR     0x01    /* Data ready (receive buffer full) */
#define UART_IER_THRI   0x02    /* Interrupt when the transmitter is empty */
#define UART_FIFO       16      /* 16550A transmit FIFO depth */

/*
 * Interrupt-driven transmit (docs/smp-plan.md stage 1e).
 *
 * QEMU paces its 16550 at the configured 115200 baud, ~87 us a character, and
 * serial_putc used to busy-wait for each one -- with the Big Kernel Lock held,
 * since every printk and every console write(2) runs in the kernel.  A
 * 60-character exec line cost 5 ms during which no other CPU could enter the
 * kernel at all.  Now output goes into tx_ring and the transmitter-empty
 * interrupt (IRQ4) feeds the UART one FIFO load at a time, so a writer only
 * copies bytes: the line still reaches the console in order, a little later.
 *
 * Synchronous output remains for the paths that cannot count on IRQ4 running
 * again: before serial_enable_async() (early boot, interrupts off), and while
 * serial_sync_begin() is in force (panic, double fault, the NMI dump of a
 * wedged machine).  Those first drain whatever is queued, so nothing is lost
 * or reordered.  A writer that finds the ring full waits for room, feeding
 * the UART itself between short holds of the lock: back-pressure, never loss
 * (under such overload two writers' bytes may interleave).  The PIT tick (serial_tx_poll) backs the interrupt up should an edge
 * ever be missed.
 *
 * tx_lock is taken with interrupts off.  Nothing is called under it, so it
 * nests inside the console lock (kernel/printk.c) and under the BKL; a CPU
 * that already holds it (an NMI landing mid-update) writes through directly.
 */
#define TX_RING 65536U                  /* power of two */
static char              tx_ring[TX_RING];
static volatile uint32_t tx_head, tx_tail;  /* free-running: write / read */
static spinlock_t        tx_lock;
static volatile int32_t  tx_owner = -1;
static int               tx_async;          /* IRQ4 drains the ring        */
static int               tx_thri;           /* IER.THRI currently enabled  */
static volatile int      tx_sync;           /* serial_sync_begin depth     */

void serial_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* Disable interrupts */
    outb(COM1 + UART_LCR, UART_LCR_DLAB); /* Enable DLAB to set baud rate */
    outb(COM1 + UART_DATA, 0x01);  /* Divisor low  byte: 1 → 115200 baud */
    outb(COM1 + UART_IER,  0x00);  /* Divisor high byte: 0 */
    outb(COM1 + UART_LCR, UART_LCR_8N1); /* 8N1, clear DLAB */
    outb(COM1 + UART_FCR, 0xC7);   /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + UART_MCR, 0x0B);   /* RTS + DSR set, IRQs enabled */
}

static inline int tx_thr_empty(void) {
    return (inb(COM1 + UART_LSR) & UART_LSR_THRE) != 0;
}

/* Blocking write of one byte, straight to the UART. */
static void tx_raw(char c) {
    while (!tx_thr_empty())
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
}

/* Under tx_lock: the transmitter is empty, so its FIFO takes a full load. */
static void tx_fill_fifo(void) {
    for (int i = 0; i < UART_FIFO && tx_tail != tx_head; i++)
        outb(COM1 + UART_DATA, (uint8_t)tx_ring[tx_tail++ % TX_RING]);
}

/* Under tx_lock: write everything queued, waiting for the UART. */
static void tx_drain_sync(void) {
    while (tx_tail != tx_head)
        tx_raw(tx_ring[tx_tail++ % TX_RING]);
}

static int tx_lock_enter(uint32_t *fl) {
    __asm__ volatile("pushf; pop %0; cli" : "=r"(*fl) :: "memory");
    if (!apic_available()) return 0;            /* one CPU: no one to race */
    int32_t me = (int32_t)this_cpu_id();
    if (tx_owner == me) return -1;              /* nested on this CPU (NMI) */
    for (uint32_t n = 0; !spin_trylock(&tx_lock); n++) {
        /* Every hold is short (one FIFO load at most), so only a panic or
         * a double fault, whose holder may have been stopped, gives up. */
        if (tx_sync && n > 20000000U) return -1;
        tlb_serve_pending();
        __asm__ volatile("pause");
    }
    tx_owner = me;
    return 1;
}

static void tx_lock_leave(uint32_t fl, int locked) {
    if (locked == 1) {
        tx_owner = -1;
        spin_unlock(&tx_lock);
    }
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

void serial_write(const char *s, uint32_t len) {
    uint32_t i = 0;
    for (;;) {
        uint32_t fl;
        int locked = tx_lock_enter(&fl);
        if (locked < 0) {                       /* cannot touch the ring */
            for (; i < len; i++) tx_raw(s[i]);
        } else if (!tx_async || tx_sync) {
            tx_drain_sync();
            for (; i < len; i++) tx_raw(s[i]);
        } else {
            while (i < len && tx_head - tx_tail < TX_RING)
                tx_ring[tx_head++ % TX_RING] = s[i++];
            /* Full: feed the UART ourselves when it has room (IRQ4 may be
             * held off on this CPU), but never wait with the lock held. */
            if (i < len && tx_thr_empty()) tx_fill_fifo();
            if (!tx_thri && tx_tail != tx_head) {
                /* Enabling THRI with the transmitter empty raises IRQ4 at
                 * once; with it busy, the interrupt comes when it empties. */
                tx_thri = 1;
                outb(COM1 + UART_IER, UART_IER_THRI);
            }
        }
        tx_lock_leave(fl, locked);
        if (i >= len) return;
        __asm__ volatile("pause");
    }
}

/* IRQ4: the transmitter has room (or the line is shared and it is not ours). */
static void serial_irq(registers_t *regs) {
    (void)regs;
    uint32_t fl;
    int locked = tx_lock_enter(&fl);
    if (locked >= 0) {
        (void)inb(COM1 + UART_IIR);             /* acknowledge THRE */
        if (tx_thr_empty()) tx_fill_fifo();
        if (tx_tail == tx_head && tx_thri) {
            tx_thri = 0;
            outb(COM1 + UART_IER, 0);
        }
    }
    tx_lock_leave(fl, locked);
}

/* From the timer tick: refill a transmitter that went empty without an
 * interrupt reaching us. */
void serial_tx_poll(void) {
    if (!tx_async || tx_tail == tx_head) return;
    uint32_t fl;
    int locked = tx_lock_enter(&fl);
    if (locked >= 0 && tx_tail != tx_head && tx_thr_empty()) tx_fill_fifo();
    tx_lock_leave(fl, locked);
}

void serial_enable_async(void) {
    /* No UART at 0x3F8 (reads float high): stay synchronous, which costs
     * nothing there anyway. */
    if (inb(COM1 + UART_LSR) == 0xFF) return;
    irq_install_handler(COM1_IRQ, serial_irq);
    pic_unmask(COM1_IRQ);
    tx_async = 1;
}

void serial_sync_begin(void) {
    __sync_add_and_fetch(&tx_sync, 1);
    uint32_t fl;
    int locked = tx_lock_enter(&fl);
    if (locked >= 0) tx_drain_sync();
    tx_lock_leave(fl, locked);
}

void serial_sync_end(void) {
    __sync_sub_and_fetch(&tx_sync, 1);
}

void serial_putc(char c) {
    serial_write(&c, 1);
}

char serial_getc(void) {
    while (!(inb(COM1 + UART_LSR) & UART_LSR_DR))
        ;
    return (char)inb(COM1 + UART_DATA);
}

int serial_data_ready(void) {
    return (inb(COM1 + UART_LSR) & UART_LSR_DR) != 0;
}

void serial_puts(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    serial_write(s, n);
}

/* Print a 32-bit value as 8 hex digits (no prefix) */
void serial_write_hex(uint32_t val) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = hex[(val >> (28 - 4 * i)) & 0xF];
    serial_write(buf, 8);
}
