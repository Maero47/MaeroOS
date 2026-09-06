#include "mouse.h"
#include "keyboard.h"
#include "../arch/i686/cpu/irq.h"
#include "../arch/i686/cpu/pic.h"
#include "../arch/i686/cpu/pit.h"
#include "../arch/i686/include/io.h"
#include "../kernel/printk.h"
#include "../proc/scheduler.h"
#include <registers.h>
#include <stdint.h>

#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_CMD_PORT        0x64
#define PS2_STATUS_OUT      0x01
#define PS2_STATUS_IN       0x02
#define PS2_STATUS_AUX      0x20

#define PS2_CMD_READ_CFG    0x20
#define PS2_CMD_WRITE_CFG   0x60
#define PS2_CMD_ENABLE_AUX  0xA8
#define PS2_CMD_MOUSE_NEXT  0xD4

#define MOUSE_CMD_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE    0xF4
#define MOUSE_ACK           0xFA

#define MOUSE_RING_SIZE     128

#define EV_REL              0x02
#define REL_X               0x00
#define REL_Y               0x01
#define REL_WHEEL           0x08
#define BTN_LEFT            0x110
#define BTN_RIGHT           0x111
#define BTN_MIDDLE          0x112

static input_event_t ring[MOUSE_RING_SIZE];
static volatile uint32_t head;
static volatile uint32_t tail;
static uint8_t packet[4];
static uint8_t packet_pos;
static uint8_t packet_size = 3;   /* 4 in IntelliMouse (wheel) mode */
static uint8_t buttons;
static int present;

static int ps2_wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_IN) == 0) return 1;
    }
    return 0;
}

static int ps2_wait_output_full(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT) return 1;
    }
    return 0;
}

static void ps2_write_cmd(uint8_t cmd) {
    if (ps2_wait_input_clear()) outb(PS2_CMD_PORT, cmd);
}

static void ps2_write_data(uint8_t data) {
    if (ps2_wait_input_clear()) outb(PS2_DATA_PORT, data);
}

static int mouse_write(uint8_t cmd) {
    ps2_write_cmd(PS2_CMD_MOUSE_NEXT);
    ps2_write_data(cmd);
    if (!ps2_wait_output_full()) return 0;
    return inb(PS2_DATA_PORT) == MOUSE_ACK;
}

static void push_event(uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (head + 1) % MOUSE_RING_SIZE;
    if (next == tail) {
        tail = (tail + 1) % MOUSE_RING_SIZE;
    }

    uint32_t ticks = pit_ticks();
    ring[head].tv_sec = (int32_t)(ticks / 100);
    ring[head].tv_usec = (int32_t)((ticks % 100) * 10000);
    ring[head].type = type;
    ring[head].code = code;
    ring[head].value = value;
    head = next;
    io_wake();
}

static void emit_button(uint8_t new_buttons, uint8_t bit,
                        uint16_t code) {
    uint8_t old_down = buttons & bit;
    uint8_t new_down = new_buttons & bit;
    if (old_down != new_down) {
        push_event(EV_KEY, code, new_down ? 1 : 0);
    }
}

static void handle_packet(void) {
    if ((packet[0] & 0x08) == 0) return;
    if (packet[0] & 0xC0) return;

    int32_t dx = (int8_t)packet[1];
    int32_t dy = -(int8_t)packet[2];
    uint8_t new_buttons = packet[0] & 0x07;
    int changed = 0;

    if (dx) {
        push_event(EV_REL, REL_X, dx);
        changed = 1;
    }
    if (dy) {
        push_event(EV_REL, REL_Y, dy);
        changed = 1;
    }
    if (packet_size == 4) {
        /* IntelliMouse Z: +1 = wheel toward user (down); Linux REL_WHEEL
         * is +1 for up, so negate. */
        int32_t dz = (int8_t)packet[3];
        if (dz) {
            push_event(EV_REL, REL_WHEEL, -dz);
            changed = 1;
        }
    }

    uint32_t before = head;
    emit_button(new_buttons, 0x01, BTN_LEFT);
    emit_button(new_buttons, 0x02, BTN_RIGHT);
    emit_button(new_buttons, 0x04, BTN_MIDDLE);
    if (head != before) changed = 1;
    buttons = new_buttons;

    if (changed) push_event(EV_SYN, SYN_REPORT, 0);
}

static void mouse_irq(registers_t *regs) {
    (void)regs;
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT) {
        uint8_t status = inb(PS2_STATUS_PORT);
        uint8_t data = inb(PS2_DATA_PORT);
        if ((status & PS2_STATUS_AUX) == 0) continue;

        if (packet_pos == 0 && (data & 0x08) == 0) continue;
        packet[packet_pos++] = data;
        if (packet_pos == packet_size) {
            handle_packet();
            packet_pos = 0;
        }
    }
}

void mouse_init(void) {
    head = tail = 0;
    packet_pos = 0;
    buttons = 0;
    present = 0;

    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT)
        (void)inb(PS2_DATA_PORT);

    ps2_write_cmd(PS2_CMD_ENABLE_AUX);

    ps2_write_cmd(PS2_CMD_READ_CFG);
    if (!ps2_wait_output_full()) {
        printk("[MOUSE] PS/2 controller config read failed\n");
        return;
    }
    uint8_t cfg = inb(PS2_DATA_PORT);
    cfg |= 0x02;
    cfg &= (uint8_t)~0x20;
    ps2_write_cmd(PS2_CMD_WRITE_CFG);
    ps2_write_data(cfg);

    if (!mouse_write(MOUSE_CMD_DEFAULTS)) {
        printk("[MOUSE] PS/2 mouse not detected\n");
        return;
    }

    /* IntelliMouse knock: sample rates 200,100,80 then identify.  Device
     * id 3 means the wheel protocol (4-byte packets) is active. */
    if (mouse_write(0xF3) && mouse_write(200) &&
        mouse_write(0xF3) && mouse_write(100) &&
        mouse_write(0xF3) && mouse_write(80) &&
        mouse_write(0xF2) && ps2_wait_output_full()) {
        uint8_t id = inb(PS2_DATA_PORT);
        if (id == 3)
            packet_size = 4;
    }

    if (!mouse_write(MOUSE_CMD_ENABLE)) {
        printk("[MOUSE] PS/2 mouse enable failed\n");
        return;
    }

    irq_install_handler(12, mouse_irq);
    pic_unmask(12);
    present = 1;
    printk("[MOUSE] PS/2 mouse driver active on IRQ12%s\n",
           packet_size == 4 ? " (wheel)" : "");
}

int mouse_has_events(void) {
    return head != tail;
}

uint32_t mouse_read_events(uint32_t len, uint8_t *buf) {
    uint32_t event_size = sizeof(input_event_t);
    uint32_t copied = 0;
    (void)present;

    while (len >= event_size && tail != head) {
        input_event_t ev = ring[tail];
        tail = (tail + 1) % MOUSE_RING_SIZE;
        __builtin_memcpy(buf + copied, &ev, event_size);
        copied += event_size;
        len -= event_size;
    }

    return copied;
}

