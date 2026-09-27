#ifndef SNES_CHEATS_H
#define SNES_CHEATS_H

#include <stdint.h>

/*
 * Cheats: hold a value at a WRAM address, every frame.
 *
 * The mechanism is not new. The script engine already has `pokefor`, which
 * re-writes a WRAM value every frame for N frames, and that path is proven -
 * it is what established that a value search is not enough (see the $25D1
 * false positive). A cheat is that same force-poke, made permanent and
 * toggleable, so nothing new had to be trusted.
 *
 * ADDRESSES ARE NOT KNOWN YET. Every entry below carries a verified flag, and
 * an unverified address cannot be armed: a cheat that writes to a guessed
 * address is not a cheat that does nothing, it is a cheat that corrupts the
 * player's city and their save. The bar shows the address and its status, so
 * the gap is visible rather than hidden behind a toggle that silently does
 * nothing.
 *
 * Finding an address is differential, not a value search. Search for the
 * number the HUD shows and you will find coincidences - $25D1 held exactly
 * 19999 and was not the treasury. Make the value MOVE (place a building, let
 * a month tick), dump WRAM before and after, and the address that changed is
 * the one; then confirm by poking it and looking at the screen.
 */
typedef struct {
  const char *id;
  const char *label;      /* what the player sees */
  const char *section;    /* game area it belongs to */
  uint32_t addr;          /* WRAM offset */
  uint8_t width;          /* 1..4 bytes */
  uint32_t value;         /* written every frame while enabled */
  int verified;           /* 1 => confirmed by poke + screenshot */
  const char *note;       /* why it is unverified, or what it does */
} SnesCheat;

int snes_cheat_count(void);
const SnesCheat *snes_cheat_at(int index);
int snes_cheat_find(const char *id);
int snes_cheat_apply(int index, int enabled);   /* 1 armed, 0 disarmed */
int snes_cheat_is_armed(int index);
void snes_cheats_frame(void);   /* call once per frame, after config parse */

/* The write, exposed so a test can exercise it on an address the read-only
 * table does not contain. A test that could only arm the shipped cheats could
 * not check the byte order at all, because none of them is verified yet. */
int snes_cheats_test_write(uint32_t addr, uint32_t value, int width);

#endif
