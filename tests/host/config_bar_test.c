/*
 * The configuration bar: layout arithmetic and click mapping.
 *
 * ROM-free. The bar reads g_config, so this test DEFINES g_config itself and
 * links against the bar alone, with the snes_ovl_* primitives stubbed out --
 * which is the point: the drawing is verified by a screenshot, and what a
 * screenshot cannot show is whether a click at a given pixel maps to the
 * action the user sees under the cursor.
 *
 * The bugs this pins are the ones that were actually made while writing it:
 *   - a 6-character button label overflows 336/7 and collides with its
 *     neighbour, so every label must fit the cell the layout computes;
 *   - a computed key column goes negative on a 336-wide field and welds the
 *     section name to the key;
 *   - snes_config_bar_move(0) is not "no move": the sign test treats it as
 *     down. Changing a value must go through step_row, not move(0);
 *   - a click that misses the bar must return kCfgAct_None, or every click in
 *     the game would be swallowed by an invisible strip.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "snes_config_bar.h"

/* The bar links against these; the real ones draw pixels, which this test does
 * not look at. Each records the last call so the layout can be interrogated. */
static int g_fill_calls, g_fill_max_w;
static int g_last_fill_x, g_last_fill_y, g_last_fill_w, g_last_fill_h;
static char g_texts[64][48];
static int g_text_n;

void snes_ovl_fill_rect(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, int w, int h, uint32_t col) {
  (void)dst; (void)stride; (void)h_max; (void)col;
  g_fill_calls++;
  if (w > g_fill_max_w) g_fill_max_w = w;
  g_last_fill_x = x0; g_last_fill_y = y0; g_last_fill_w = w; g_last_fill_h = h;
}
void snes_ovl_stroke_rect(uint32_t *dst, int stride, int h_max,
                          int x, int y, int w, int h, uint32_t col) {
  (void)dst; (void)stride; (void)h_max; (void)x; (void)y; (void)w; (void)h; (void)col;
}
void snes_ovl_upscale_frame(uint8_t *dst, int pitch, int dst_w, int dst_h,
                            const uint32_t *src, int src_pitch,
                            int src_w, int src_h) {
  (void)dst; (void)pitch; (void)dst_w; (void)dst_h;
  (void)src; (void)src_pitch; (void)src_w; (void)src_h;
}
void snes_ovl_fill_disc(uint32_t *dst, int stride, int h_max,
                        int cx, int cy, int r, uint32_t col) {
  (void)dst; (void)stride; (void)h_max; (void)cx; (void)cy; (void)r; (void)col;
}
void snes_ovl_draw_char(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, char c, uint32_t col, int scale) {
  (void)dst; (void)stride; (void)h_max; (void)x0; (void)y0; (void)c; (void)col; (void)scale;
}
void snes_ovl_draw_text(uint32_t *dst, int stride, int h_max,
                        int x, int y, const char *s, uint32_t col, int scale) {
  (void)dst; (void)stride; (void)h_max; (void)x; (void)y; (void)col; (void)scale;
  if (g_text_n < 64 && s) { snprintf(g_texts[g_text_n], sizeof g_texts[0], "%s", s); g_text_n++; }
}
void snes_ovl_draw_button(uint32_t *dst, int stride, int h_max,
                          int x, int y, char label, uint32_t col) {
  (void)dst; (void)stride; (void)h_max; (void)x; (void)y; (void)label; (void)col;
}
void snes_ovl_blit_panel(uint8_t *dst, int pitch, int dst_w, int dst_h,
                         const uint32_t *panel, int panel_w, int panel_h) {
  (void)dst; (void)pitch; (void)dst_w; (void)dst_h;
  (void)panel; (void)panel_w; (void)panel_h;
}
void snes_ovl_blit_panel_rect(uint8_t *dst, int pitch, int dst_w, int dst_h,
                              const uint32_t *panel, int panel_w, int panel_h,
                              int rx, int ry, int rw, int rh) {
  (void)dst; (void)pitch; (void)dst_w; (void)dst_h;
  (void)panel; (void)panel_w; (void)panel_h; (void)rx; (void)ry; (void)rw; (void)rh;
}

/* The bar holds no state of its own, so the test IS the host: a values array
 * and two accessors. This is the whole reason the bar takes values by index
 * instead of including config.h. */
static int g_values[64];
static int g_set_calls, g_set_last_index, g_set_last_value;

static int tb_get(int idx) { return (idx >= 0 && idx < 64) ? g_values[idx] : 0; }
static void tb_set(int idx, int v) {
  g_set_calls++;
  g_set_last_index = idx;
  g_set_last_value = v;
  if (idx >= 0 && idx < 64) g_values[idx] = v;
}

static int g_fail;
static void check(int cond, const char *what) {
  if (!cond) { printf("FAIL: %s\n", what); g_fail++; }
  else printf("ok: %s\n", what);
}

static void reset(uint8_t *fb, int w, int h) {
  memset(g_values, 0, sizeof g_values);
  g_values[1] = 3;    /* window scale */
  g_values[12] = 100; /* volume */
  g_set_calls = 0;
  g_fill_max_w = 0;
  g_text_n = 0;
  snes_config_bar_init(NULL, tb_get, tb_set);
  (void)fb; (void)w; (void)h;
}

int main(void) {
  static uint8_t fb[336 * 224 * 4];
  int w = 336, h = 224;
  int i;

  /* ---- the bar is up without being asked for ------------------------- */
  reset(fb, w, h);
  check(!snes_config_bar_expanded(), "bar defaults to the compact form");
  snes_config_bar_draw(fb, w * 4, w, h);
  check(g_fill_calls > 0, "compact bar draws something by default");
  check(g_text_n > 0, "compact bar writes text by default");
  check(g_fill_max_w == w, "something spans the full frame width");

  /* ---- every button label fits its cell ------------------------------- */
  {
    int cells = 7, cw = w / cells, over = 0;
    /* The labels are the ones the bar draws; mirrored here so a future label
     * edit that overruns is caught by this test rather than by a screenshot. */
    static const char *const labels[] = { "PAUSE", "VOL-", "VOL+", "SAVES", "REW", "SHOT", "PERF" };
    for (i = 0; i < cells; i++) {
      int need = (int)strlen(labels[i]) * 8;
      if (need > cw - 2) {
        printf("      label '%s' needs %dpx, cell is %dpx\n", labels[i], need, cw);
        over++;
      }
    }
    check(over == 0, "every command label fits its button cell");
  }

  /* ---- a click on each button returns that button's action ------------- */
  {
    static const SnesConfigBarAction expect[] = {
      kCfgAct_Pause, kCfgAct_VolDown, kCfgAct_VolUp, kCfgAct_States,
      kCfgAct_Rewind, kCfgAct_Screenshot, kCfgAct_Perf
    };
    int cells = 7, cw = w / cells;
    for (i = 0; i < cells; i++) {
      SnesConfigBarAction a = snes_config_bar_click(i * cw + cw / 2, 14);
      char msg[80];
      snprintf(msg, sizeof msg, "click in cell %d hits that button", i);
      check(a == expect[i], msg);
      if (a != expect[i]) printf("      got %d want %d\n", (int)a, (int)expect[i]);
    }
  }

  /* ---- a click below the bar reaches the game -------------------------- */
  {
    SnesConfigBarAction a = snes_config_bar_click(168, 200);
    check(a == kCfgAct_None, "a click on the field is not swallowed by the bar");
  }
  {
    SnesConfigBarAction a = snes_config_bar_click(-5, 14);
    check(a == kCfgAct_None, "a click off the left edge is not swallowed");
  }

  /* ---- the expanded list selects and edits ---------------------------- */
  reset(fb, w, h);
  snes_config_bar_toggle_expanded();
  check(snes_config_bar_expanded(), "F1 expands the bar");
  snes_config_bar_draw(fb, w * 4, w, h);

  {
    int row = snes_config_bar_row_at(10, 60);
    check(row >= 0, "a click inside the list hits a row");
    if (row >= 0) {
      check(snes_config_bar_select_row(row), "the hit row can be selected");
      check(snes_config_bar_selected() == row, "selection follows the click");
    }
  }

  /* Widescreen is the first row and is live: left/right must reach the host. */
  {
    int before = g_values[0];
    check(snes_config_bar_step_row(0, +1), "a live row accepts a change");
    check(g_set_calls == 1, "changing a live row stores exactly once");
    check(g_set_last_index == 0, "the store names the row that was shown");
    check(g_values[0] != before, "a bool flips");
  }

  /* A bounded integer clamps rather than running away, and a cycle wraps: a
   * cycle that clamped would make "next" a silent no-op on the last value. */
  {
    snes_config_bar_select_row(12);            /* VOLUME, 0..100, step 5 */
    snes_config_bar_step_row(12, -1); snes_config_bar_step_row(12, -1);
    check(g_values[12] == 90, "volume steps down by its magnitude");
    snes_config_bar_step_row(12, +1);
    check(g_values[12] == 95, "volume steps back up");
    snes_config_bar_select_row(1);             /* WINDOWSCALE, a 6-entry cycle */
    snes_config_bar_step_row(1, -1);
    check(g_values[1] == 2, "a cycle steps back one entry");
  }

  /* move(0) must not be a movement: the sign test would read it as "down". */
  {
    int before = snes_config_bar_selected();
    snes_config_bar_move(0);
    check(snes_config_bar_selected() == before, "move(0) is not a movement");
  }
  {
    int before = snes_config_bar_selected();
    snes_config_bar_move(+1);
    check(snes_config_bar_selected() == before + 1, "move(+1) goes down one");
    snes_config_bar_move(-1);
    check(snes_config_bar_selected() == before, "move(-1) goes back up");
  }

  /* ---- the list stays inside the panel at both ends -------------------- */
  {
    int i2;
    for (i2 = 0; i2 < 200; i2++) snes_config_bar_move(+1);
    check(snes_config_bar_selected() < 200, "the selection cannot run off the end");
    snes_config_bar_draw(fb, w * 4, w, h);
    check(snes_config_bar_row_at(10, 60) >= 0, "a row is still under the cursor at the end");
  }

  /* ---- the live / restart-only split ---------------------------------- */
  {
    int n = snes_config_bar_row_count(), live = 0, ro = 0, k;
    check(n > 20, "the table covers the config surface, not a token few");
    for (k = 0; k < n; k++) {
      if (snes_config_bar_row_editable(k)) live++; else ro++;
    }
    check(live > 0, "some options are editable without a restart");
    check(ro > 0, "some options are honestly marked restart-only");
    /* Widescreen is the first row and must be live: it is the one the player
     * most wants, and it is the whole point of the bar. */
    check(snes_config_bar_row_editable(0), "widescreen is live-editable");
  }

  /* ---- a restart-only row refuses to change ---------------------------- */
  {
    int n = snes_config_bar_row_count(), k, tested = 0;
    for (k = 0; k < n; k++) {
      int before;
      if (snes_config_bar_row_editable(k)) continue;
      before = g_values[k];
      g_set_calls = 0;
      check(!snes_config_bar_step_row(k, +1), "a restart-only row refuses the change");
      check(g_set_calls == 0, "a refused change stores nothing");
      check(g_values[k] == before, "a restart-only value is untouched");
      tested++;
      break;
    }
    check(tested == 1, "there was a restart-only row to test");
  }

  /* ---- out-of-range indices are refused, not crashes ------------------- */
  {
    check(!snes_config_bar_row_editable(-1), "index -1 is not editable");
    check(!snes_config_bar_row_editable(9999), "a far index is not editable");
    check(!snes_config_bar_select_row(9999), "a far row cannot be selected");
    check(!snes_config_bar_step_row(-1, +1), "stepping a bad row is refused");
  }

  if (g_fail) { printf("\n%d FAILURE(S)\n", g_fail); return 1; }
  printf("\nconfig bar: all checks passed\n");
  return 0;
}
