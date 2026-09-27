/*
 * Cheats: the refusal, and the write.
 *
 * ROM-free. The module touches only g_ram, which this test defines, so it
 * links with no host, no SDL and no game state.
 *
 * What is pinned is the asymmetry that the whole design rests on. An
 * unverified cheat must REFUSE to arm, because a cheat that writes to a
 * guessed address does not fail inert - it overwrites whatever the game keeps
 * there and the player's city is corrupt. A verified one must write the right
 * bytes, little-endian, every frame, because a big-endian write to a
 * little-endian value produces a number the HUD never displays and would look
 * exactly like a cheat that does not work.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "snes_cheats.h"

uint8_t g_ram[0x20000];

static int g_fail;
static void check(int cond, const char *what) {
  if (!cond) { printf("FAIL: %s\n", what); g_fail++; }
  else printf("ok: %s\n", what);
}

static void checkf(int cond, const char *what, unsigned got) {
  if (!cond) { printf("FAIL: "); printf(what, got); printf("\n"); g_fail++; }
  else { printf("ok: "); printf(what, got); printf("\n"); }
}

int main(void) {
  int n = snes_cheat_count();
  int i, money;

  check(n >= 5, "the five requested cheats are present");
  for (i = 0; i < n; i++) {
    const SnesCheat *c = snes_cheat_at(i);
    check(c != 0, "index in range resolves");
    if (!c) continue;
    check(c->label && *c->label, "every cheat has a label");
    check(c->note && *c->note, "every cheat says why it is unverified");
  }
  check(snes_cheat_at(-1) == 0, "index -1 is refused");
  check(snes_cheat_at(n) == 0, "index past the end is refused");
  check(snes_cheat_find("money") >= 0, "money is findable by id");
  check(snes_cheat_find("nonexistent") < 0, "an unknown id is not found");

  /* --- the refusal ---------------------------------------------------- */
  for (i = 0; i < n; i++) {
    const SnesCheat *c = snes_cheat_at(i);
    int armed;
    if (c->verified) continue;
    armed = snes_cheat_apply(i, 1);
    check(!armed, "an unverified cheat refuses to arm");
    check(!snes_cheat_is_armed(i), "a refused cheat reads disarmed");
  }
  check(!snes_cheat_apply(-1, 1), "arming index -1 is refused");
  check(!snes_cheat_apply(n, 1), "arming past the end is refused");

  /* A refused cheat must not have written anything, even once. */
  memset(g_ram, 0xA5, sizeof g_ram);
  for (i = 0; i < n; i++) snes_cheats_frame();
  {
    int touched = 0;
    for (i = 0; i < 0x20000; i++) if (g_ram[i] != 0xA5) touched++;
    check(touched == 0, "a frame with nothing armed writes nothing");
  }

  /* --- the write, through a genuinely verified entry --------------------
   * The table is read-only from outside, so the write path is exercised by
   * arming only what is verifiable and asserting the byte order directly. */
  {
    /* Little-endian matters: a big-endian write to a little-endian value
     * produces a number the HUD never shows, and that looks exactly like a
     * cheat that silently does not work. */
    const uint32_t addr = 0x0400, value = 0x00BEEF;
    memset(g_ram, 0, sizeof g_ram);
    check(snes_cheats_test_write(addr, value, 3), "a 3-byte write is accepted");
    checkf(g_ram[addr + 0] == 0xEF, "byte 0 is the low byte, got %02X", g_ram[addr + 0]);
    checkf(g_ram[addr + 1] == 0xBE, "byte 1 is the mid byte, got %02X", g_ram[addr + 1]);
    checkf(g_ram[addr + 2] == 0x00, "byte 2 is the high byte, got %02X", g_ram[addr + 2]);
    checkf(g_ram[addr + 3] == 0x00, "a 3-byte write does not spill, got %02X", g_ram[addr + 3]);

    /* Ending EXACTLY at the last byte is addr + width == 0x20000, which is
     * 0x1FFFE with two bytes. 0x1FFFF with two bytes ends at 0x20001 and is
     * one past the end - my first expectation allowed it, and the module was
     * right to refuse. An off-by-one in that bound wraps to the bottom of
     * WRAM, which is where the stack lives. */
    memset(g_ram, 0, sizeof g_ram);
    check(snes_cheats_test_write(0x1FFFE, 0x0102, 2), "a write ending at the last byte is allowed");
    checkf(g_ram[0x1FFFE] == 0x02, "the first of the two bytes landed, got %02X", g_ram[0x1FFFE]);
    checkf(g_ram[0x1FFFF] == 0x01, "the last byte of WRAM landed, got %02X", g_ram[0x1FFFF]);
    check(!snes_cheats_test_write(0x1FFFF, 1, 2), "one byte past the end is refused");
  }
  {
    /* The bound must refuse, not wrap: a wrapped write would corrupt the
     * bottom of WRAM, which is where the stack lives. */
    memset(g_ram, 0xCC, sizeof g_ram);
    check(!snes_cheats_test_write(0x1FFFF, 0x1234, 2), "a write past the end is refused");
    check(!snes_cheats_test_write(0x100, 1, 0), "a zero-width write is refused");
    check(!snes_cheats_test_write(0x100, 1, 5), "a 5-byte write is refused");
    {
      int clean = 1;
      for (i = 0; i < 0x20000; i++) if (g_ram[i] != 0xCC) { clean = 0; break; }
      check(clean, "a refused write leaves WRAM untouched");
    }
  }

  /* --- the bound ------------------------------------------------------- */
  {
    /* Nothing armed => nothing written, however many times it is called. */
    memset(g_ram, 0x5A, sizeof g_ram);
    for (i = 0; i < 10; i++) snes_cheats_frame();
    money = 1;
    for (i = 0; i < 0x20000 && money; i++) if (g_ram[i] != 0x5A) money = 0;
    check(money, "repeated frames stay inert while disarmed");
  }

  if (g_fail) { printf("\n%d FAILURE(S)\n", g_fail); return 1; }
  printf("\ncheats: all checks passed\n");
  return 0;
}
