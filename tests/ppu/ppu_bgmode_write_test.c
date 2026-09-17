/* Synthetic regression for the $2105 (BGMODE) write path.
 *
 * Real hardware treats BGMODE as a full byte: bits 0-2 = BG mode, bit 3 =
 * BG1 priority, bits 4-7 = tile-size select for BG1-BG4 (bsnes io.cpp:
 * bgN.io.tileSize = data >> (4+N) & 1). Games legitimately set 16x16 tiles,
 * which is exactly what PPU_bigTiles feeds the big-tile renderers. A port
 * that asserted "(val & 0xf0) == 0" on the write aborted the moment SimCity's
 * attract loop switched a layer to 16x16 ($FB = mode 3, BG1 256-colour, all
 * four BGs big) — a host abort on genuine hardware traffic.
 *
 * No game ROM, generated data, or platform frontend is required. */
#include <stdio.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

/* ppu.c reaches runtime globals defined in snes.c / common_rtl.c and the
 * ws_shadow streaming-tilemap helpers. This harness links ppu.c on its own;
 * none of them participate in the BGMODE write path. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t hscroll, uint16_t map_word_offset,
                      uint16_t real_tile) {
    (void)layer; (void)screen_x; (void)wrapped_y; (void)hscroll;
    (void)map_word_offset;
    return real_tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer; (void)screen_x; return 0;
}
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr; (void)value;
}

static int failures;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

int main(void) {
    Ppu *ppu = ppu_init();
    if (!ppu)
        return 2;
    ppu_reset(ppu);

    /* Whole byte, every BG at 16x16 — the exact write SimCity's attract loop
     * performs ($FB). Must be stored, never aborted. */
    ppu_write(ppu, 0x05, 0xFB);
    check(ppu->bgmode == 0xFB, "BGMODE write $FB is stored in full");
    for (int layer = 0; layer < 4; layer++)
        check(PPU_bigTiles(ppu, layer) != 0, "all four BGs read as 16x16 tiles");

    /* Only BG1 big (mode 3 + BG1 256-colour + BG1 tile size). */
    ppu_write(ppu, 0x05, 0x1B);
    check(ppu->bgmode == 0x1B, "BGMODE write $1B is stored");
    check(PPU_bigTiles(ppu, 0) != 0, "BG1 reads 16x16");
    check(PPU_bigTiles(ppu, 1) == 0 && PPU_bigTiles(ppu, 2) == 0 &&
              PPU_bigTiles(ppu, 3) == 0,
          "BG2-BG4 stay 8x8");

    /* Plain mode-0 start byte and $FF boundary: no abort either way. */
    ppu_write(ppu, 0x05, 0x00);
    check(ppu->bgmode == 0x00, "BGMODE write $00 is stored");
    ppu_write(ppu, 0x05, 0xFF);
    check(ppu->bgmode == 0xFF, "BGMODE write $FF is stored");
    for (int layer = 0; layer < 4; layer++)
        check(PPU_bigTiles(ppu, layer) != 0, "$FF selects 16x16 on every BG");

    if (failures == 0) {
        printf("ppu_bgmode_write_test: PASS\n");
        return 0;
    }
    printf("ppu_bgmode_write_test: FAIL (%d)\n", failures);
    return 1;
}