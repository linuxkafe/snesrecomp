#include "joypad.h"
#include "snes.h"
#include "saveload.h"

#include <string.h>

/*
 * All multitap state lives here rather than in `struct Snes`, for two reasons:
 * snes_saveload serialises raw struct tails whose layout is frozen, and the
 * runner already treats the machine as a singleton (g_snes, g_ppu, g_dma).
 * The savestate carries this as its own versioned chunk instead.
 */
typedef struct SnesJoypads {
    uint16_t pad[SNES_MAX_PLAYERS];       /* live 12-bit button state */
    uint8_t  connected[SNES_MAX_PLAYERS];
    uint8_t  tap[SNES_CONTROLLER_PORTS];  /* multitap present on this port */
    uint8_t  iobit[SNES_CONTROLLER_PORTS];/* $4201 bit 6 / bit 7 */
    uint16_t latched[SNES_MAX_PLAYERS];
    /* Shift counters. A standard controller ignores IOBit and uses bank 0
     * only; a tap keeps one counter per bank, so switching banks mid-frame
     * resumes that bank where it left off rather than restarting. */
    uint8_t  index[SNES_CONTROLLER_PORTS][2];
    uint16_t auto_word[4];                /* $4218, $421A, $421C, $421E */
    uint8_t  auto_valid;

    /* SNES Mouse per port: KJOY_DEV_PAD or KJOY_DEV_MOUSE; host motion
     * accumulating between strobe edges; the latched snapshot the guest shifts
     * out; the shift and speed counters; read diagnostics for the probe. */
    uint8_t  dev[SNES_CONTROLLER_PORTS];
    int      mouse_pend_x[SNES_CONTROLLER_PORTS];
    int      mouse_pend_y[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_pend_l[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_pend_r[SNES_CONTROLLER_PORTS];
    int      mouse_x[SNES_CONTROLLER_PORTS];
    int      mouse_y[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_l[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_r[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_shift[SNES_CONTROLLER_PORTS];
    uint8_t  mouse_speed[SNES_CONTROLLER_PORTS];
    uint32_t read_count[SNES_CONTROLLER_PORTS];
    uint8_t  max_shift[SNES_CONTROLLER_PORTS];
    uint32_t auto_read_count[SNES_CONTROLLER_PORTS];
} SnesJoypads;

static SnesJoypads g_jp;

void joypad_reset_state(void)
{
    uint8_t tap0 = g_jp.tap[0];
    uint8_t tap1 = g_jp.tap[1];
    uint8_t dev0 = g_jp.dev[0];
    uint8_t dev1 = g_jp.dev[1];
    int i;

    memset(&g_jp, 0, sizeof(g_jp));
    /* Port configuration is host setup, not guest state: a reset unplugs
     * nothing. A mouse survives a reset and keeps its pending motion. */
    g_jp.tap[0] = tap0;
    g_jp.tap[1] = tap1;
    g_jp.dev[0] = dev0;
    g_jp.dev[1] = dev1;
    for (i = 0; i < joypad_player_count(); i++)
        g_jp.connected[i] = 1;
}

/* ── seat mapping ────────────────────────────────────────────────────── */

int joypad_player_count(void)
{
    if (g_jp.tap[0] && g_jp.tap[1])
        return 8;
    if (g_jp.tap[0] || g_jp.tap[1])
        return 5;
    return 2;
}

/*
 * Logical pad reported on `line` (0 = Data1, 1 = Data2) of `port`, for the
 * currently selected bank. Returns -1 when nothing drives that line.
 */
static int seat_for(unsigned port, int line)
{
    int bank;

    if (port >= SNES_CONTROLLER_PORTS)
        return -1;

    if (!g_jp.tap[port]) {
        /* A standard controller drives Data1 only. */
        if (line != 0)
            return -1;
        if (g_jp.tap[0] && port == 1)
            return 4;          /* tap on port 1 -> lone pad on port 2 is seat 4 */
        if (g_jp.tap[1] && port == 0)
            return 0;          /* tap on port 2 -> lone pad on port 1 is seat 0 */
        return (int)port;      /* no taps at all */
    }

    /* IOBit 1 selects the tap's first pad pair, IOBit 0 the second. */
    bank = g_jp.iobit[port] ? 0 : 1;
    {
        int slot = bank * 2 + line;      /* 0..3 within this tap */
        int base;
        if (port == 0)
            base = 0;                    /* tap on port 1 always owns 0..3 */
        else
            base = g_jp.tap[0] ? 4 : 1;  /* dual tap -> 4..7, else 1..4 */
        return base + slot;
    }
}

/* Which shift counter a port's reads advance right now. */
static int bank_for(unsigned port)
{
    if (!g_jp.tap[port])
        return 0;
    return g_jp.iobit[port] ? 0 : 1;
}

/* ── host input ──────────────────────────────────────────────────────── */

void joypad_set_pad(int slot, uint16_t buttons)
{
    if (slot < 0 || slot >= SNES_MAX_PLAYERS)
        return;
    g_jp.pad[slot] = buttons & 0x0fffu;
}

uint16_t joypad_get_pad(int slot)
{
    if (slot < 0 || slot >= SNES_MAX_PLAYERS)
        return 0;
    return g_jp.pad[slot];
}

void joypad_set_connected(int slot, int connected)
{
    if (slot < 0 || slot >= SNES_MAX_PLAYERS)
        return;
    g_jp.connected[slot] = connected ? 1u : 0u;
}

int joypad_get_connected(int slot)
{
    if (slot < 0 || slot >= SNES_MAX_PLAYERS)
        return 0;
    return g_jp.connected[slot] ? 1 : 0;
}

void joypad_set_multitap(int port, int enabled)
{
    int i;

    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return;
    if (g_jp.tap[port] == (enabled ? 1u : 0u))
        return;
    g_jp.tap[port] = enabled ? 1u : 0u;
    /* A tap owns the port's Data1 line, so it unplugs a mouse there. */
    if (enabled && g_jp.dev[port] == KJOY_DEV_MOUSE) {
        g_jp.dev[port] = KJOY_DEV_PAD;
        g_jp.read_count[port] = 0;
        g_jp.max_shift[port] = 0;
    }

    /* Seats that only exist because of this tap have never been plugged in
     * as far as `connected` is concerned, and an unplugged seat reports 0 on
     * the 17th bit — so a game that enables a tap after boot would find every
     * new slot empty. Occupied is the useful default; a game that wants an
     * empty slot says so with joypad_set_connected AFTER configuring ports,
     * which is the order docs/MULTITAP.md documents. */
    for (i = 0; i < joypad_player_count(); i++)
        g_jp.connected[i] = 1;
}

int joypad_get_multitap(int port)
{
    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return 0;
    return g_jp.tap[port] ? 1 : 0;
}

/* ── SNES Mouse ──────────────────────────────────────────────────────── */

void joypad_set_device(int port, int device)
{
    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return;
    if (device != KJOY_DEV_PAD && device != KJOY_DEV_MOUSE)
        return;
    /* A tap already owns the port's Data1 line, so the mouse is refused
     * rather than shadowed. */
    if (device == KJOY_DEV_MOUSE && g_jp.tap[port])
        return;
    if (g_jp.dev[port] == device)
        return;
    g_jp.dev[port] = (uint8_t)device;
    if (device == KJOY_DEV_MOUSE) {
        g_jp.mouse_pend_x[port] = 0;
        g_jp.mouse_pend_y[port] = 0;
        g_jp.mouse_pend_l[port] = 0;
        g_jp.mouse_pend_r[port] = 0;
        g_jp.mouse_x[port] = 0;
        g_jp.mouse_y[port] = 0;
        g_jp.mouse_shift[port] = 0;
        g_jp.mouse_speed[port] = 0;
    }
    g_jp.read_count[port] = 0;
    g_jp.max_shift[port] = 0;
    g_jp.auto_read_count[port] = 0;
}

int joypad_get_device(int port)
{
    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return KJOY_DEV_PAD;
    return g_jp.dev[port] == KJOY_DEV_MOUSE ? KJOY_DEV_MOUSE : KJOY_DEV_PAD;
}

int joypad_set_mouse(int port, int dx, int dy, int left, int right)
{
    int px, py;

    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return 0;
    if (g_jp.dev[port] != KJOY_DEV_MOUSE)
        return 0;
    px = g_jp.mouse_pend_x[port] + dx;
    py = g_jp.mouse_pend_y[port] + dy;
    if (px > 127) px = 127;
    if (px < -127) px = -127;
    if (py > 127) py = 127;
    if (py < -127) py = -127;
    g_jp.mouse_pend_x[port] = px;
    g_jp.mouse_pend_y[port] = py;
    g_jp.mouse_pend_l[port] = left ? 1u : 0u;
    g_jp.mouse_pend_r[port] = right ? 1u : 0u;
    return 1;
}

uint32_t joypad_read_count(int port)
{
    if (port < 0 || port >= SNES_CONTROLLER_PORTS)
        return 0;
    return g_jp.read_count[port];
}

uint8_t joypad_max_shift(int port)
{
    if (port < 0 || port > 1)
        return 0;
    return g_jp.max_shift[port];
}

uint32_t joypad_auto_read_count(int port)
{
    if (port < 0 || port > 1)
        return 0;
    return g_jp.auto_read_count[port];
}

uint16_t joypad_auto_word_visible(int port, int data_line)
{
    int slot, w;
    if (port < 0 || port > 1 || data_line < 0 || data_line > 1)
        return 0;
    slot = data_line * 2 + port;
    w = g_jp.auto_word[slot];
    if (!g_jp.auto_valid)
        return 0;
    return (uint16_t)w;
}

/* ── $4201 / $4213 ───────────────────────────────────────────────────── */

void joypad_write_iobit(Snes *snes, uint8_t wrio)
{
    (void)snes;
    g_jp.iobit[0] = (wrio & 0x40u) ? 1u : 0u;
    g_jp.iobit[1] = (wrio & 0x80u) ? 1u : 0u;
}

uint8_t joypad_read_iobit(void)
{
    return (uint8_t)((g_jp.iobit[0] ? 0x40u : 0u) |
                     (g_jp.iobit[1] ? 0x80u : 0u));
}

/* ── latch / strobe ──────────────────────────────────────────────────── */

/* One serial bit of the SNES Mouse stream, from the reference emulator's
 * table (bsnes sfc/controller/mouse/mouse.cpp). Index 0 is the first bit a
 * game reads after the strobe drops; the movement fields carry the snapshot
 * taken when the strobe latched. */
static uint8_t mouse_bit(unsigned port, uint8_t idx)
{
    int x = g_jp.mouse_x[port];
    int y = g_jp.mouse_y[port];
    int ay = y < 0 ? -y : y;
    int ax = x < 0 ? -x : x;

    if (idx < 8) return 0;                 /* sync */
    switch (idx) {
    case 8:  return g_jp.mouse_r[port];    /* right button */
    case 9:  return g_jp.mouse_l[port];    /* left button */
    case 10: return (uint8_t)((g_jp.mouse_speed[port] >> 1) & 1u);
    case 11: return (uint8_t)((g_jp.mouse_speed[port] >> 0) & 1u);
    case 12: case 13: case 14: return 0;
    case 15: return 1;                     /* signature: 0001 (pad is 0000) */
    case 16: return y < 0 ? 1u : 0u;       /* sign-y: 1 = up */
    case 24: return x < 0 ? 1u : 0u;       /* sign-x: 1 = left */
    default:
        if (idx <= 23) return (uint8_t)((ay >> (23 - idx)) & 1u);
        if (idx <= 31) return (uint8_t)((ax >> (31 - idx)) & 1u);
        return 0;
    }
}

static void mouse_latch(unsigned port, int consume)
{
    int x, y, scale;

    g_jp.mouse_shift[port] = 0;
    if (!consume)
        return; /* rising edge / automatic read: freeze only */

    /* bsnes scales the movement read-out by the speed value the guest cycled
     * while the strobe was held (1.0 / 1.5 / 2.0). Integer-exact scale: the
     * speed index selects (2 + speed) / 2. */
    scale = 2 + g_jp.mouse_speed[port];
    x = (g_jp.mouse_pend_x[port] * scale) / 2;
    y = (g_jp.mouse_pend_y[port] * scale) / 2;
    if (x > 127) x = 127;
    if (x < -127) x = -127;
    if (y > 127) y = 127;
    if (y < -127) y = -127;
    g_jp.mouse_x[port] = x;
    g_jp.mouse_y[port] = y;
    g_jp.mouse_l[port] = g_jp.mouse_pend_l[port];
    g_jp.mouse_r[port] = g_jp.mouse_pend_r[port];

    /* The falling edge owns one frame of host motion. */
    g_jp.mouse_pend_x[port] = 0;
    g_jp.mouse_pend_y[port] = 0;
    g_jp.mouse_pend_l[port] = 0;
    g_jp.mouse_pend_r[port] = 0;
}

/*
 * `strobe_high` is the port strobe value at this edge: 1 on the rising edge
 * and while it is held (snapshot/reset only), 0 on the falling edge (the
 * guest is about to shift, so a mouse consumes its pending motion). The pad
 * paths are edge-agnostic and unchanged.
 */
static void joypad_latch(Snes *snes, int strobe_high)
{
    int i;

    /*
     * Seat authority. With no multitap the two-pad fields the rest of the
     * runtime already writes (RtlRunFrame -> input1/input2_currentState) stay
     * authoritative, so nothing about the existing two-player path changes
     * and a caller that pokes those fields directly still works.
     *
     * With a tap the seat numbering no longer lines up with the physical
     * ports at all — a tap on port 2 makes seat 1 the tap's FIRST pad, not
     * the port-2 controller — so pad[] set through joypad_set_pad is the
     * only authority.
     */
    if (snes && !g_jp.tap[0] && !g_jp.tap[1]) {
        g_jp.pad[0] = (uint16_t)(snes->input1_currentState & 0x0fffu);
        g_jp.pad[1] = (uint16_t)(snes->input2_currentState & 0x0fffu);
    }

    for (i = 0; i < SNES_MAX_PLAYERS; i++)
        g_jp.latched[i] = g_jp.pad[i];
    memset(g_jp.index, 0, sizeof(g_jp.index));

    for (i = 0; i < SNES_CONTROLLER_PORTS; i++)
        if (g_jp.dev[i] == KJOY_DEV_MOUSE)
            mouse_latch((unsigned)i, strobe_high ? 0 : 1);

    if (snes) {
        snes->joypad1Latched = g_jp.latched[0];
        snes->joypad2Latched = g_jp.latched[1];
        snes->joypad1Index = 0;
        snes->joypad2Index = 0;
    }
}

void joypad_write_strobe(Snes *snes, uint8_t value)
{
    int next = (value & 1u) != 0;

    if (!snes)
        return;
    if (next || snes->joypadStrobe)
        joypad_latch(snes, next);
    snes->joypadStrobe = next ? true : false;
}

/* ── serial reads ────────────────────────────────────────────────────── */

/*
 * Live (unlatched) state of a seat, following the same authority rule as
 * joypad_latch: with no tap the two fields the rest of the runtime writes
 * stay authoritative, so a caller that pokes input1/input2_currentState
 * between a strobe and a read still sees its own value — which is what the
 * hardware does and what tests/joypad/manual_joypad_test.c pins.
 */
static uint16_t live_pad(Snes *snes, int seat)
{
    if (snes && !g_jp.tap[0] && !g_jp.tap[1]) {
        if (seat == 0) return (uint16_t)(snes->input1_currentState & 0x0fffu);
        if (seat == 1) return (uint16_t)(snes->input2_currentState & 0x0fffu);
    }
    return g_jp.pad[seat];
}

/*
 * One data line of a port at shift position `idx`. Pure: the caller owns the
 * counter, because a single read clocks BOTH lines and must sample them at
 * the same position — advancing inside here would hand Data2 the bit after
 * Data1's.
 */
static uint8_t read_line(Snes *snes, unsigned port, int line, uint8_t idx)
{
    int seat = seat_for(port, line);

    if (seat < 0)
        return 0; /* nothing drives Data2 of a port with no tap */

    if (snes->joypadStrobe) {
        /* While the latch is held the tap identifies itself: 0 on Data1 and
         * 1 on Data2, so the documented eight-read detection sees $FF. A
         * plain pad keeps reporting its live first button. */
        if (g_jp.tap[port])
            return line == 0 ? 0u : 1u;
        return (uint8_t)(live_pad(snes, seat) & 1u);
    }

    if (idx < 16)
        return (uint8_t)((g_jp.latched[seat] >> idx) & 1u);
    if (idx == 16 && g_jp.tap[port])
        return g_jp.connected[seat] ? 1u : 0u; /* 17th bit: seat occupied */
    return 1u;
}

uint8_t joypad_read_port(Snes *snes, unsigned port)
{
    int bank;
    uint8_t idx;
    uint8_t d0, d1;

    if (!snes || port >= SNES_CONTROLLER_PORTS)
        return 1;

    /* A mouse owns the port's Data1 line outright. While the strobe is held
     * it stays quiet and cycles its speed; when released it shifts the
     * bsnes table out on Data1 with Data2 resting at 0. */
    if (g_jp.dev[port] == KJOY_DEV_MOUSE) {
        g_jp.read_count[port]++;
        if (snes->joypadStrobe) {
            g_jp.mouse_speed[port] = (uint8_t)((g_jp.mouse_speed[port] + 1) % 3);
            return 0;
        }
        if (g_jp.mouse_shift[port] < 32) {
            uint8_t bit = mouse_bit(port, g_jp.mouse_shift[port]);
            uint8_t n = (uint8_t)(g_jp.mouse_shift[port] + 1u);
            g_jp.mouse_shift[port]++;
            if (n > g_jp.max_shift[port])
                g_jp.max_shift[port] = n;
            return bit;
        }
        return 1; /* reads past bit 31 report a connected device */
    }

    bank = bank_for(port);
    idx = g_jp.index[port][bank];
    d0 = read_line(snes, port, 0, idx);
    d1 = read_line(snes, port, 1, idx);

    if (!snes->joypadStrobe) {
        if (idx < 0xffu)
            g_jp.index[port][bank] = (uint8_t)(idx + 1u);
        if (!g_jp.tap[port]) {
            /* Keep the legacy per-port fields the savestate carries in step. */
            uint8_t *legacy = port ? &snes->joypad2Index : &snes->joypad1Index;
            if (*legacy < 16)
                (*legacy)++;
        }
    }
    return (uint8_t)(d0 | (d1 << 1));
}

/* Legacy single-line entry point. Retained because tests/joypad and any
 * out-of-tree caller still speak it; it is joypad_read_port's Data1. */
uint8_t joypad_read_serial(Snes *snes, unsigned port)
{
    return (uint8_t)(joypad_read_port(snes, port) & 1u);
}

/* ── automatic read ──────────────────────────────────────────────────── */

uint16_t joypad_auto_read_word(uint16_t state)
{
    uint16_t word = 0;
    int i;
    for (i = 0; i < 16; i++, state >>= 1)
        word = (uint16_t)(word * 2u + (state & 1u));
    return word;
}

uint8_t joypad_auto_read_reg(uint16_t state, unsigned reg)
{
    uint16_t word = joypad_auto_read_word(state);
    return (uint8_t)((reg & 1u) ? (word >> 8) : (word & 0xffu));
}

/* The 16-bit prefix of the mouse stream in hardware register order (first
 * read at the MSB, matching what the automatic read clocks into $4218+). */
static uint16_t mouse_auto_word(unsigned port)
{
    uint16_t w = 0;
    int i;
    for (i = 0; i < 16; i++)
        w = (uint16_t)(w | ((uint16_t)mouse_bit(port, (uint8_t)i) << (15 - i)));
    return w;
}

void joypad_auto_read(Snes *snes)
{
    int port;

    if (!snes)
        return;
    /* The automatic read performs its own latch and sixteen clocks. */
    joypad_latch(snes, 1); /* freezes a mouse; a mouse never reads here */

    for (port = 0; port < SNES_CONTROLLER_PORTS; port++) {
        int line;
        g_jp.auto_read_count[port]++;
        for (line = 0; line < 2; line++) {
            int seat;
            int slot = line * 2 + port;
            if (g_jp.dev[port] == KJOY_DEV_MOUSE) {
                /* A mouse shifts its 16-bit ID prefix into the auto register
                 * on Data1; Data2 stays 0, as it always is for a mouse. */
                g_jp.auto_word[slot] =
                    line == 0 ? mouse_auto_word((unsigned)port) : 0u;
                continue;
            }
            seat = seat_for((unsigned)port, line);
            /* $4218 = port1 Data1, $421A = port2 Data1,
             * $421C = port1 Data2, $421E = port2 Data2. */
            g_jp.auto_word[slot] =
                seat < 0 ? 0u : joypad_auto_read_word(g_jp.latched[seat]);
        }
        /* Sixteen clocks land the selected bank's counter on the 17th bit,
         * which is exactly where the documented five-player sequence expects
         * to find the connection flag. The unselected bank stays at 0. */
        g_jp.index[port][bank_for((unsigned)port)] = 16;
    }
    if (snes) {
        snes->joypad1Index = 16;
        snes->joypad2Index = 16;
    }
    g_jp.auto_valid = 1;
}

uint8_t joypad_auto_read_reg_addr(Snes *snes, uint16_t reg)
{
    int slot;

    if (reg < 0x4218u || reg > 0x421fu)
        return 0;
    slot = (int)((reg - 0x4218u) >> 1);

    /*
     * With no multitap anywhere, answer exactly as the pre-multitap runner
     * did: straight from the live pad state, with no dependence on whether
     * the automatic-read handshake has run yet. Every shipping two-player
     * port is on this path, and it must not acquire a new failure mode
     * (a game that reads $4218 without enabling automatic read used to get
     * live data and would otherwise start getting zeroes).
     */
    if (!g_jp.tap[0] && !g_jp.tap[1]) {
        switch (slot) {
        case 0:
            return joypad_auto_read_reg(
                (uint16_t)(snes ? snes->input1_currentState : 0), reg);
        case 1:
            if (g_jp.dev[1] == KJOY_DEV_MOUSE) {
                uint16_t w = mouse_auto_word(1);
                return (uint8_t)((reg & 1u) ? (w >> 8) : (w & 0xffu));
            }
            return joypad_auto_read_reg(
                (uint16_t)(snes ? snes->input2_currentState : 0), reg);
        default:
            return 0; /* nothing on Data2 */
        }
    }

    if (!g_jp.auto_valid)
        return 0;
    return (uint8_t)((reg & 1u) ? (g_jp.auto_word[slot] >> 8)
                                : (g_jp.auto_word[slot] & 0xffu));
}

/* ── savestate / digest ──────────────────────────────────────────────── */

void joypad_saveload(SaveLoadInfo *sli)
{
    if (!sli)
        return;
    /* Shift/latch state is guest-visible mid-frame: a state saved between two
     * halves of a five-player read must resume in the same bank, at the same
     * bit. Live pad state is host input and is refreshed every frame, but it
     * rides along so a loaded state is self-consistent before the first
     * RtlRunFrame. */
    sli->func(sli, &g_jp, sizeof(g_jp));
}

const void *joypad_state_blob(size_t *size_out)
{
    if (size_out)
        *size_out = sizeof(g_jp);
    return &g_jp;
}
