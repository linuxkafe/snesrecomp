#ifndef SNES_JOYPAD_H
#define SNES_JOYPAD_H

#include <stdint.h>
#include <stddef.h>

struct Snes;
struct SaveLoadInfo;

/*
 * SNES controller ports, with optional Super Multitap (Hudson HUD-101).
 *
 * Hardware model, from the SNESdev wiki "Multitap" page and the Super Famicom
 * development wiki "Controllers" page:
 *
 *   - Each controller port carries TWO serial data lines. Reads of $4016
 *     (port 1) and $4017 (port 2) return Data1 in bit 0 and Data2 in bit 1.
 *     A standard controller drives Data1 only.
 *
 *   - $4201 is an open-collector output whose bit 6 drives port 1's IOBit pin
 *     and bit 7 drives port 2's. A standard controller ignores IOBit; a
 *     multitap uses it to choose which PAIR of its four pads it reports:
 *     IOBit 1 selects the first pair (onto Data1/Data2), IOBit 0 the second.
 *     Bit 7 is also the PPU counter-latch line, which is why $4201 already
 *     had a meaning here before multitap existed.
 *
 *   - A tap reports 17 bits per pad: the usual 16, then one more that is 1
 *     when a controller is plugged into that tap slot and 0 when it is empty.
 *     Reads past that return 1s.
 *
 *   - Detection, per the wiki: with the $4016 strobe HIGH a tap reports 1 on
 *     Data2 (so eight reads give $FF); with the strobe low it reports pad
 *     data, which is $FF only if all eight of those buttons are held. A port
 *     with no tap reports 0 on Data2 and so never looks like one.
 *
 * Logical pad numbering follows psxrecomp's multitap (runtime/include/sio.h)
 * so the two engines describe the same seat in the same words:
 *
 *   no tap          port 1 -> pad 0, port 2 -> pad 1
 *   tap on port 1   tap -> pads 0..3, port 2 -> pad 4
 *   tap on port 2   port 1 -> pad 0, tap -> pads 1..4   (the 5-player layout)
 *   taps on both    port 1 tap -> pads 0..3, port 2 tap -> pads 4..7
 *
 * With no tap enabled every path here is byte-identical to the two-pad
 * behaviour that shipped before multitap: see joypad_read_port and
 * joypad_auto_word.
 */

#define SNES_MAX_PLAYERS 8
#define SNES_CONTROLLER_PORTS 2
#define SNES_TAP_PADS 4

/* Live pad state + shift/latch machinery. Singleton, like g_snes / g_ppu. */
void joypad_reset_state(void);

/* Host input. Slots outside 0..SNES_MAX_PLAYERS-1 are ignored. */
void joypad_set_pad(int slot, uint16_t buttons);
uint16_t joypad_get_pad(int slot);
void joypad_set_connected(int slot, int connected);
int  joypad_get_connected(int slot);

/* Multitap presence per physical port (0 = port 1, 1 = port 2). */
void joypad_set_multitap(int port, int enabled);
int  joypad_get_multitap(int port);
/* Highest logical pad the current port configuration can reach: 2, 5, or 8. */
int  joypad_player_count(void);

/*
 * SNES Mouse (peripheral) plugged into a controller port.
 *
 * Unlike the multitap a mouse is a DEVICE on the port's Data1 line, not a
 * logical pad seat, and a game reads it the same way it reads a controller:
 * strobe the port, then shift out. The wire protocol is the reference
 * emulator's (bsnes sfc/controller/mouse/mouse.cpp): 32 data bits instead of
 * 16, a 0001 device signature in bits 12-15 (a pad's is 0000), two speed bits
 * that cycle while the strobe is held and scale the movement read-out, and
 * sign+magnitude movement for each axis. Reads past bit 31 report a connected
 * device (1), as a pad does past bit 15. Data2 carries nothing.
 *
 * Host input: the host feeds per-frame motion/buttons with joypad_set_mouse;
 * deltas accumulate until the strobe's FALLING edge latches them (the point
 * where the guest is actually reading), so a frozen guest never loses input
 * and a held strobe never re-consumes the same motion more than once.
 *
 * A mouse and a multitap on the same physical port are mutually exclusive —
 * the later configuration wins. Enablement is host setup (like the tap), so
 * joypad_reset_state keeps the device in place.
 */
#define KJOY_DEV_PAD 0
#define KJOY_DEV_MOUSE 1

void joypad_set_device(int port, int device);
int  joypad_get_device(int port);
/* Feed one frame of host motion (screen pixels, right/up positive) and
 * buttons (left/right physical). Returns 1 if accepted, 0 if the port has no
 * mouse plugged in. Deltas saturate at 127 so a burst cannot overflow. */
int  joypad_set_mouse(int port, int dx, int dy, int left, int right);

/* Read diagnostics for the SNESRECOMP_PAD_PROBE host knob: how many manual
 * reads the guest has made on a port since the device was configured, and the
 * deepest shift position those reads reached (0 when nothing read it). A game
 * driving a mouse shifts past 24; a pad game stops at 16. */
uint32_t joypad_read_count(int port);
uint8_t  joypad_max_shift(int port);
uint32_t joypad_auto_read_count(int port);
uint16_t joypad_auto_word_visible(int port, int data_line);

/* $4201 IOBit outputs: bit 6 -> port 1, bit 7 -> port 2. */
void joypad_write_iobit(struct Snes *snes, uint8_t wrio);
/* $4213 readback of the same lines. */
uint8_t joypad_read_iobit(void);

void joypad_write_strobe(struct Snes *snes, uint8_t value);

/* $4016 / $4017 read: Data1 in bit 0, Data2 in bit 1. */
uint8_t joypad_read_port(struct Snes *snes, unsigned port);

/*
 * Automatic joypad read. Called when the hardware handshake begins (vblank);
 * latches the pads, fills the four $4218-$421F words, and leaves the manual
 * shift counters where 16 clocks would leave them — which is what lets the
 * documented 5-player sequence read the 17th connection bit straight after
 * the automatic read completes.
 */
void joypad_auto_read(struct Snes *snes);
/* $4218-$421F. reg is the register address. */
uint8_t joypad_auto_read_reg_addr(struct Snes *snes, uint16_t reg);

/* Savestate chunk (format v8+). */
void joypad_saveload(struct SaveLoadInfo *sli);
/* Raw state for the netplay rollback digest — guest-visible shift/latch
 * state that must agree between peers. */
const void *joypad_state_blob(size_t *size_out);

/* Pure helpers retained from the pre-multitap implementation; still used by
 * SwapInputBits and pinned by tests/joypad/auto_joypad_test.c. */
uint16_t joypad_auto_read_word(uint16_t state);
uint8_t joypad_auto_read_reg(uint16_t state, unsigned reg);
uint8_t joypad_read_serial(struct Snes *snes, unsigned port);

#endif /* SNES_JOYPAD_H */
