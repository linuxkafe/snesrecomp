#include <string.h>

#include "snes_cheats.h"

/* g_ram is the same flat 128KB region cpu->ram aliases (common_rtl.h), so a
 * cheat writes the bytes the guest actually reads. Declared here rather than
 * included so this module stays free of desktop headers and testable alone. */
extern uint8_t g_ram[0x20000];

/*
 * The five cheats asked for. All five are UNVERIFIED, and that is the honest
 * state of the work, not a placeholder to be tidied up later:
 *
 *   addr 0 with verified 0 means "not located yet". snes_cheat_apply refuses
 *   to arm such an entry, so a player cannot corrupt a city by toggling one.
 *
 * $25D1 is recorded as a WARNING rather than a cheat. It held exactly 19999,
 * the treasury the HUD displays, and poking 0x1234 at it did not move the
 * number on screen - a value search finds coincidences, and that one cost a
 * full round trip to establish.
 */
static const SnesCheat g_cheats[] = {
  /* The one cheat that is verified end to end. Difficulty is a free variable
   * that changes the starting treasury by a known amount ($20000 vs $10000),
   * and two runs differing only in that respect differ by 41 bytes of WRAM at
   * the same frame. Holding 0xBEEF here for the whole city shows $51800 -
   * 48879 plus the income the game added on top - WITH the game's own
   * treasury-rising marker lit, so the game believes the number rather than
   * merely displaying it. $0B9C, one byte below, also moves the display but
   * is a derived field: it settles at 11700 whatever is written, so it is not
   * the master and must not be used. */
  { "money",      "INFINITE MONEY",  "TREASURY", 0x0B9D, 4, 0x0000BEEF, 1,
    "verified: $20000/$10000 by difficulty, confirmed by a held poke" },
  { "specials",   "SPECIAL BLDGS",   "BUILDING", 0, 0, 0, 0,
    "a bitmask in the building-unlock table; not located" },
  { "pollution",  "NO POLLUTION",    "CITY",     0, 0, 0, 0,
    "likely a per-tile or per-zone accumulator summed each tick" },
  { "crime",      "NO CRIME",        "CITY",     0, 0, 0, 0,
    "the crime rate the approval icon reads" },
  { "traffic",    "NO TRAFFIC",      "CITY",     0, 0, 0, 0,
    "the traffic volume the approval icon reads" },
};
static const int g_cheat_count_ = (int)(sizeof(g_cheats) / sizeof(g_cheats[0]));

static uint8_t g_armed[sizeof(g_cheats) / sizeof(g_cheats[0])];

int snes_cheat_count(void) { return g_cheat_count_; }

const SnesCheat *snes_cheat_at(int index) {
  if (index < 0 || index >= g_cheat_count_) return 0;
  return &g_cheats[index];
}

int snes_cheat_find(const char *id) {
  int i;
  if (!id) return -1;
  for (i = 0; i < g_cheat_count_; i++)
    if (strcmp(g_cheats[i].id, id) == 0) return i;
  return -1;
}

int snes_cheat_apply(int index, int enabled) {
  if (index < 0 || index >= g_cheat_count_) return 0;
  /* The refusal is the point. Writing to an unlocated address does not leave
   * the cheat inert, it corrupts whatever the game keeps there. */
  if (enabled && (!g_cheats[index].verified || g_cheats[index].width == 0))
    return 0;
  g_armed[index] = enabled ? 1 : 0;
  return 1;
}

int snes_cheat_is_armed(int index) {
  if (index < 0 || index >= g_cheat_count_) return 0;
  return g_armed[index];
}

static int cheat_write(uint32_t addr, uint32_t value, int width) {
  int k;
  if (width < 1 || width > 4) return 0;
  if (addr + (uint32_t)width > 0x20000u) return 0;   /* refuse, do not wrap */
  for (k = 0; k < width; k++)
    g_ram[addr + k] = (uint8_t)((value >> (8 * k)) & 0xFFu);
  return 1;
}

int snes_cheats_test_write(uint32_t addr, uint32_t value, int width) {
  return cheat_write(addr, value, width);
}

void snes_cheats_frame(void) {
  int i;
  for (i = 0; i < g_cheat_count_; i++) {
    const SnesCheat *c = &g_cheats[i];
    if (!g_armed[i]) continue;
    if (!c->verified || c->width == 0) continue;
    (void)cheat_write(c->addr, c->value, c->width);
  }
}
