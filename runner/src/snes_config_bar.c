#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "snes_config_bar.h"

/*
 * The bar owns PRESENTATION and nothing else: the option names, their bounds
 * and enums, the layout, and the pixel-to-action mapping. The host owns STATE
 * and hands values in and out by index through two function pointers.
 *
 * That split is not tidiness for its own sake. config.h pulls in SDL, so a
 * module that included it could not be linked by the standalone test harness;
 * and a bar that reached into g_config itself would have to be re-audited
 * every time a key moved. With the state behind two pointers, the test below
 * drives the whole layout with a static array and no SDL at all.
 */

#define BAR_BG        0xFF101418u   /* near-black, not opaque black: the field
                                      underneath is game art, and a pure black
                                      strip reads as a hole in the picture */
#define BAR_EDGE      0xFF3A4654u
#define BAR_DIM       0xFF7C8894u   /* restart-only rows: present, not editable */
#define BAR_TEXT      0xFFE6EDF3u
#define BAR_HILITE    0xFF2C4A6Bu   /* the selected row */
#define BAR_OK        0xFF6BE06Bu

#define ROW_H 9                      /* 8px glyph + 1px leading */
#define FONT_W 8
#define KEY_COL 21                   /* key column width, in characters */
#define COMPACT_H (ROW_H * 2 + 3)

static SnesConfigBarHooks g_hooks;
static int (*g_get)(int index);
static void (*g_set)(int index, int value);
/* RENDERER is enumerated by the HOST at runtime, not hardcoded here. SDL
 * offers whatever render drivers the build and the machine have, and vulkan is
 * one of them - a fixed list silently hid it, which is the whole point of
 * asking for it. The bar asks the host for the names and the count. */
static int g_renderer_count;
static const char *(*g_renderer_name)(int index);
static int (*g_renderer_current)(void);
static void (*g_renderer_choose)(int index);
static const char *(*g_cheat_note)(int cheat_index);
static int g_expanded;
/* F1 opens the bar; it is not up by default. The bar covers 21 of the field's
 * 224 rows, and a player who never changes a setting should not pay for that
 * on every frame of play. */
static int g_visible;
static int g_sel;
static int g_first;
static int g_layout_w;
static int g_row_y[80];
static int g_row_n;

typedef enum { kOpt_Bool, kOpt_Int, kOpt_Enum } OptKind;

typedef struct {
  const char *section;
  const char *key;          /* the config.ini spelling */
  OptKind kind;
  const char *const *names; /* non-NULL => the value cycles these */
  int name_count;
  int step_mag, lo, hi;
  int editable;             /* 0 => read once at boot, greyed with RESTART */
} Opt;

static const char *const kAspectNames[]   = { "AUTO", "4:3", "16:9", "16:10" };
/* Up to kMaxWindowScale (10) in host_main.c, not 6. The bar was capping
 * below the host's own ceiling, so a 4K display - where the host allows
 * 9x - could not be reached from the bar. The host clamps to the display
 * regardless; naming the real range is honest, and the row wraps. */
static const char *const kScaleNames[]    = {
  "1X","2X","3X","4X","5X","6X","7X","8X","9X","10X" };
static const char *const kFullNames[]     = { "WINDOW", "FULL", "DESKTOP" };

#define O(sec, key, kind, names, n, mag, lo, hi, ed) \
  { sec, key, kind, names, n, mag, lo, hi, ed }

/* `editable` is not a guess. Each key was traced to the line that reads it: a
 * key read inside the present path or the per-frame setup can be written while
 * the game runs, and a key read once during window creation or boot cannot.
 * The evidence is next to the rows it decided.
 *
 *   vsync             host_main.c:1928  inside the present call      live
 *   display_aspect    host_main.c:1984  inside the present call      live
 *   linear_filtering  host_main.c:1951  SdlRenderer_BeginDraw        live
 *   no_sprite_limits  host_main.c:2650  PPU flags, every frame       live
 *   run_ahead         host_main.c:2427  live-settings struct         live
 *   disable_frame_delay / ignore_aspect_ratio / gamepad_deadzone /
 *   skip_launcher     read per frame in the same loop                live
 *   display_perf_title host_main.c:3033  boot only                   RESTART
 *   fullscreen         host_main.c:2334  window creation             RESTART
 *   audio freq/samples/channels, output method, keymap, gamepad map,
 *   netplay name      read while the device is opened               RESTART
 */
static Opt g_options[] = {
  O("GRAPHICS", "WIDESCREEN",     kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "WINDOWSCALE",     kOpt_Enum, kScaleNames,    10, 1, 0, 9, 1),
  O("GRAPHICS", "DISPLAYASPECT",   kOpt_Enum, kAspectNames,    4, 1, 0, 3, 1),
  O("GRAPHICS", "RENDERER",        kOpt_Enum, NULL,         0, 1, 0, 0, 1),
  O("GRAPHICS", "SHADER",          kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("GRAPHICS", "LINEARFILTERING", kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "NOSPRITELIMITS",  kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "FRAMEBLEND",      kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "VSYNC",           kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "FULLSCREEN",      kOpt_Enum, kFullNames,      3, 1, 0, 2, 0),
  O("GRAPHICS", "IGNORASPECTRATIO",kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GRAPHICS", "NEWRENDERER",     kOpt_Bool, NULL,            0, 1, 0, 1, 0),

  O("SOUND", "VOLUME",             kOpt_Int,  NULL,            0, 5, 0, 100, 1),
  O("SOUND", "ENABLEAUDIO",        kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("SOUND", "AUDIOFREQ",          kOpt_Int,  NULL,            0, 1, 0, 65535, 0),
  O("SOUND", "AUDIOSAMPLES",       kOpt_Int,  NULL,            0, 1, 0, 65535, 0),
  O("SOUND", "AUDIOCHANNELS",      kOpt_Int,  NULL,            0, 1, 0, 8, 0),
  O("SOUND", "OUTPUTMETHOD",       kOpt_Int,  NULL,            0, 1, 0, 2, 0),

  O("EMULATION", "RUNAHEAD",       kOpt_Int,  NULL,            0, 1, 0, 8, 1),
  O("EMULATION", "DISABLEFRAMEDELAY",kOpt_Bool,NULL,            0, 1, 0, 1, 1),
  O("EMULATION", "REWINDGESTURE",  kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("EMULATION", "AUTOSAVE",       kOpt_Bool, NULL,            0, 1, 0, 1, 0),

  O("GENERAL", "DISPLAYPERFTITLE", kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("GENERAL", "SKIPLAUNCHER",     kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("GENERAL", "PLAYERNAME",       kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("GENERAL", "GAMEPADDEADZONE",  kOpt_Int,  NULL,            0, 5, 0, 100, 1),

  O("CONTROLLER", "SOURCEP1",      kOpt_Int,  NULL,            0, 1, 0, 2, 0),
  O("CONTROLLER", "SOURCEP2",      kOpt_Int,  NULL,            0, 1, 0, 2, 0),
  O("CONTROLLER", "GAMEPADP1",      kOpt_Bool, NULL,            0, 1, 0, 1, 0),
  O("CONTROLLER", "GAMEPADP2",      kOpt_Bool, NULL,            0, 1, 0, 1, 0),

  /* The cheats. Present, not hidden: a bar that omitted five toggles the
   * player was told about would read as broken. The address status travels
   * with the value, because "OFF" and "cannot work yet" are different facts
   * and a toggle that silently does nothing is the failure mode worth
   * engineering against. */
  O("CHEATS", "INFINITE MONEY",     kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("CHEATS", "SPECIAL BLDGS",      kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("CHEATS", "NO POLLUTION",       kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("CHEATS", "NO CRIME",           kOpt_Bool, NULL,            0, 1, 0, 1, 1),
  O("CHEATS", "NO TRAFFIC",         kOpt_Bool, NULL,            0, 1, 0, 1, 1),

  /* The soft mouse lives in the bar because the alternative was an environment
   * variable, which a player cannot see and a launcher cannot set. It is
   * genuinely live - the mapping is per-frame input, so arming it needs no
   * restart and no reload. */
  O("INPUT", "SOFT MOUSE",          kOpt_Bool, NULL,            0, 1, 0, 1, 1),
};
static const int g_option_count = (int)(sizeof(g_options) / sizeof(g_options[0]));

/* The command buttons, in drawn order. Labels are <= 5 chars: the bar divides
 * the width by the count, and 336/7 = 48px per button minus the 2px inset
 * leaves 46 usable, so a 6-char (48px) label overflows into its neighbour. The
 * first screenshot of the bar is what caught that. */
static const struct { const char *label; SnesConfigBarAction act; } kButtons[] = {
  { "PAUSE",  kCfgAct_Pause },
  { "VOL-",   kCfgAct_VolDown },
  { "VOL+",   kCfgAct_VolUp },
  { "SAVES",  kCfgAct_States },
  { "REW",    kCfgAct_Rewind },
  { "SHOT",   kCfgAct_Screenshot },
  { "PERF",   kCfgAct_Perf },
};
static const int kButtonCount = (int)(sizeof(kButtons) / sizeof(kButtons[0]));

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void put(char *dst, int *col, const char *s) {
  while (*s) dst[(*col)++] = *s++;
  dst[*col] = 0;
}

static void put_int(char *dst, int *col, int v) {
  char tmp[12]; int n = 0;
  if (v == 0) { dst[(*col)++] = '0'; dst[*col] = 0; return; }
  if (v < 0) { dst[(*col)++] = '-'; v = -v; }
  while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
  while (n > 0) dst[(*col)++] = tmp[--n];
  dst[*col] = 0;
}

static int opt_value(int idx) { return g_get ? g_get(idx) : 0; }

static void opt_value_str(const Opt *o, int idx, char *out) {
  int v = opt_value(idx), c = 0;
  if (o->kind == kOpt_Enum) {
    put(out, &c, (o->names && v >= 0 && v < o->name_count) ? o->names[v] : "?");
    return;
  }
  if (o->kind == kOpt_Bool) { put(out, &c, v ? "ON" : "OFF"); return; }
  put_int(out, &c, v);
}

void snes_config_bar_set_cheat_note(const char *(*note)(int cheat_index)) {
  g_cheat_note = note;
}

void snes_config_bar_set_renderers(int count,
                                   const char *(*name)(int index),
                                   int (*current)(void),
                                   void (*choose)(int index)) {
  g_renderer_count = count;
  g_renderer_name = name;
  g_renderer_current = current;
  g_renderer_choose = choose;
}

void snes_config_bar_init(const SnesConfigBarHooks *hooks,
                          int (*get_value)(int index),
                          void (*set_value)(int index, int value)) {
  const char *e;
  if (hooks) g_hooks = *hooks;
  g_get = get_value;
  g_set = set_value;
  /* SNESRECOMP_CONFIG_BAR=1 starts visible with the full list open. It exists
   * so a headless screenshot can reach the expanded layout, which is otherwise
   * only reachable by pressing a key on a machine that has a keyboard, and it
   * is also the escape hatch for anyone who wants the bar up permanently now
   * that F1 opens it on demand. */
  e = getenv("SNESRECOMP_CONFIG_BAR");
  g_visible = g_expanded = (e && *e == '1') ? 1 : 0;
  g_sel = 0;
  /* SNESRECOMP_CONFIG_BAR_SEL=<n>: start the selection on row n. The bar's nav
   * reads the real keyboard, which a headless run has no way to press, so
   * without this the lower sections are unreachable in a screenshot and would
   * only ever be verified by reading the source. */
  g_first = 0;
  g_layout_w = 0;
  /* AFTER the g_first reset, not before: the original reset sat on the next
   * line and silently undid this, which is why a selection of 31 still
   * rendered from the top of the list. */
  e = getenv("SNESRECOMP_CONFIG_BAR_SEL");
  if (e) {
    int n = atoi(e);
    if (n >= 0 && n < g_option_count) { g_sel = n; g_first = n; }
  }
}

void snes_config_bar_toggle_expanded(void) { g_expanded = !g_expanded; }
/* F1: hidden becomes the full list, the full list becomes hidden. Opening
 * shows the list rather than the one-line summary, because the summary's only
 * job is to say that a key exists, and if you pressed the key you know it
 * exists. */
void snes_config_bar_toggle_visible(void) {
  if (!g_visible) { g_visible = 1; g_expanded = 1; }
  else            { g_visible = 0; g_expanded = 0; }
}
int  snes_config_bar_visible(void) { return g_visible; }
int  snes_config_bar_expanded(void) { return g_expanded; }
int  snes_config_bar_selected(void) { return g_sel; }
int  snes_config_bar_row_count(void) { return g_option_count; }
int  snes_config_bar_row_editable(int idx) {
  if (idx < 0 || idx >= g_option_count) return 0;
  return g_options[idx].editable;
}

/* Clamp for a bounded integer, wrap for a cycle. A cycle that clamped at its
 * ends would make "next" a no-op on the last value, which reads as a broken
 * key rather than as the end of a list. */
static int next_value(const Opt *o, int cur, int dir) {
  int span = o->hi - o->lo + 1;
  int v = cur;
  if (o->kind == kOpt_Bool) v = cur ? 0 : 1;
  else if (o->kind == kOpt_Enum || span <= 1) {
    int n = o->name_count ? o->name_count : span;
    v = cur + (dir > 0 ? 1 : n - 1);
    v = o->lo + ((v - o->lo) % n + n) % n;
  } else {
    v = clampi(cur + dir * o->step_mag, o->lo, o->hi);
  }
  return v;
}

int snes_config_bar_step_row(int idx, int dir) {
  const Opt *o;
  if (idx < 0 || idx >= g_option_count) return 0;
  o = &g_options[idx];
  if (!o->editable) return 0;
  if (idx == 3 && g_renderer_choose && g_renderer_count > 0) {
    int cur = g_renderer_current();
    int nxt = cur + (dir > 0 ? 1 : g_renderer_count - 1);
    if (nxt < 0) nxt = 0;
    if (nxt >= g_renderer_count) nxt = g_renderer_count - 1;
    g_sel = idx;
    g_renderer_choose(nxt);
    return 1;
  }
  if (!g_set) return 0;
  g_sel = idx;
  g_set(idx, next_value(o, opt_value(idx), dir));
  return 1;
}

int snes_config_bar_move(int dir) {
  int next;
  /* dir is a SIGN, not a delta: 0 is neither up nor down, and treating it as
   * down would silently move the selection for any caller that passed a
   * computed direction. Callers that want "no movement" want selected(). */
  if (dir == 0) return g_sel;
  next = g_sel + (dir < 0 ? -1 : 1);
  if (next < 0) next = 0;
  if (next >= g_option_count) next = g_option_count - 1;
  g_sel = next;
  return g_sel;
}

int snes_config_bar_select_row(int idx) {
  if (idx < 0 || idx >= g_option_count) return 0;
  g_sel = idx;
  return 1;
}

static void summary_line(char *out) {
  int c = 0;
  char v[16];
  out[0] = 0;
  put(out, &c, "SCALE ");
  {
    int s = clampi(opt_value(1), 0, 10);
    if (s >= 10) { out[c++] = '1'; out[c++] = '0'; out[c] = 0; }
    else if (s > 0) { out[c++] = (char)('0' + s); out[c] = 0; }
  }
  put(out, &c, "  ASPECT ");
  opt_value_str(&g_options[2], 2, v);
  put(out, &c, v);
  put(out, &c, "  WIDE ");
  put(out, &c, opt_value(0) ? "ON" : "OFF");
  /* Volume is deliberately absent: it has two dedicated buttons, and a number
   * that changes under the cursor is harder to read than to press. */
  put(out, &c, "  F1=CONFIG");
}

static void draw_buttons(uint32_t *px, int w, int y, int h_max) {
  int bw = w / kButtonCount, i;
  if (bw <= 0) return;
  for (i = 0; i < kButtonCount; i++) {
    int x = i * bw;
    snes_ovl_fill_rect(px, w, h_max, x + 1, y, bw - 2, ROW_H + 1, BAR_HILITE);
    snes_ovl_stroke_rect(px, w, h_max, x + 1, y, bw - 2, ROW_H + 1, BAR_EDGE);
    snes_ovl_draw_text(px, w, h_max,
                       x + (bw - (int)strlen(kButtons[i].label) * FONT_W) / 2,
                       y + 1, kButtons[i].label, BAR_TEXT, 1);
  }
}

/* `pitch` is BYTES per row, but the drawing primitives take a stride in uint32
 * ELEMENTS, and dst_w is that stride. Passing pitch/4 would be wrong on any
 * frame whose rows are padded, so the primitives get dst_w and pitch is only
 * meaningful to the blit_panel family, which this bar does not use. */
void snes_config_bar_draw(uint8_t *dst, int pitch, int dst_w, int dst_h) {
  uint32_t *px = (uint32_t *)dst;
  char line[160], v[16];
  int i;
  (void)pitch;

  if (dst_w <= 0 || dst_h <= 0 || !g_get) return;
  if (!g_visible) return;
  g_layout_w = dst_w;

  snes_ovl_fill_rect(px, dst_w, dst_h, 0, 0, dst_w, COMPACT_H, BAR_BG);
  snes_ovl_fill_rect(px, dst_w, dst_h, 0, COMPACT_H - 1, dst_w, 1, BAR_EDGE);

  summary_line(line);
  if ((int)strlen(line) >= dst_w / FONT_W) line[dst_w / FONT_W - 1] = 0;
  snes_ovl_draw_text(px, dst_w, dst_h, 2, 1, line, BAR_TEXT, 1);
  draw_buttons(px, dst_w, ROW_H + 1, dst_h);

  if (!g_expanded) { g_row_n = 0; return; }

  {
    int px0 = 2, py0 = COMPACT_H + 2;
    int pw = dst_w - 4, ph = dst_h - py0 - 2;
    int list_y, rows, vis;

    snes_ovl_fill_rect(px, dst_w, dst_h, px0, py0, pw, ph, BAR_BG);
    snes_ovl_stroke_rect(px, dst_w, dst_h, px0, py0, pw, ph, BAR_EDGE);
    snes_ovl_draw_text(px, dst_w, dst_h, px0 + 3, py0 + 2, "CONFIGURATION", BAR_OK, 1);
    snes_ovl_draw_text(px, dst_w, dst_h, px0 + pw - 3 - 8 * FONT_W, py0 + 2,
                       "F1 CLOSE", BAR_DIM, 1);

    list_y = py0 + 13;
    rows = (ph - 13 - 10) / ROW_H;
    if (rows < 3) rows = 3;
    vis = rows - 2;                       /* footer, plus room for a header */

    if (g_sel < g_first) g_first = g_sel;
    if (g_sel > g_first + vis - 1) g_first = g_sel - vis + 1;
    if (g_first < 0) g_first = 0;

    g_row_n = 0;
    {
      /* A section header is drawn when the first visible option belongs to a
       * new section. Keeping headers out of the index means one is never
       * stranded at the bottom with its options scrolled off. */
      const char *shown = NULL;
      int max_rows = g_option_count - g_first;
      if (max_rows > vis - 1) max_rows = vis - 1;
      for (i = 0; i < max_rows && g_row_n < vis; i++) {
        int idx = g_first + i, y, c = 0, pad;
        const Opt *o = &g_options[idx];

        if (!shown || strcmp(shown, o->section) != 0) {
          shown = o->section;
          y = list_y + g_row_n * ROW_H;
          snes_ovl_draw_text(px, dst_w, dst_h, px0 + 3, y, shown, BAR_OK, 1);
          g_row_y[g_row_n] = y;
          g_row_n++;
          if (g_row_n >= vis) break;
        }

        y = list_y + g_row_n * ROW_H;
        if (g_sel == idx)
          snes_ovl_fill_rect(px, dst_w, dst_h, px0 + 2, y - 1, pw - 4, ROW_H, BAR_HILITE);

        line[0] = 0;
        put(line, &c, " ");
        put(line, &c, o->key);
        /* A fixed key column, not a computed one: a computed pad goes negative
         * on a 336-wide field, which is how "GRAPHICSWIDESCREEN" happened. */
        pad = KEY_COL - c;
        while (pad-- > 0) line[c++] = ' ';
        line[c] = 0;
        put(line, &c, "= ");
        if (idx == 3 && g_renderer_name && g_renderer_count > 0) {
          int ri = g_renderer_current();
          put(line, &c, (ri >= 0 && ri < g_renderer_count) ? g_renderer_name(ri) : "?");
        } else opt_value_str(o, idx, v);
        put(line, &c, v);
        if (!o->editable) put(line, &c, "  RESTART");
        else if (idx >= 30) {
          /* A cheat row: append the address status. The bar does not link
           * against snes_cheats, so the host supplies this string through the
           * optional cheat_note hook; a NULL hook leaves the row alone. */
          const char *note = g_cheat_note ? g_cheat_note((int)idx - 30) : 0;
          if (note) { line[c] = ' '; line[c] = 0; put(line, &c, note); }
        }

        if ((int)strlen(line) > pw / FONT_W - 1) line[pw / FONT_W - 1] = 0;
        snes_ovl_draw_text(px, dst_w, dst_h, px0 + 3, y, line,
                           o->editable ? BAR_TEXT : BAR_DIM, 1);
        g_row_y[g_row_n] = y;
        g_row_n++;
      }
    }

    snes_ovl_draw_text(px, dst_w, dst_h, px0 + 3, py0 + ph - 9,
                       "UP DOWN PICK   LEFT RIGHT SET", BAR_DIM, 1);
  }
}

int snes_config_bar_contains(int x, int y) {
  if (x < 0 || y < 0 || !g_get) return 0;
  if (y < COMPACT_H) return 1;
  return g_expanded;
}

int snes_config_bar_row_at(int x, int y) {
  int idx;
  (void)x;   /* every row spans the panel, so y alone identifies it */
  if (!g_expanded) return -1;
  for (idx = 0; idx < g_row_n; idx++)
    if (y >= g_row_y[idx] && y < g_row_y[idx] + ROW_H) return g_first + idx;
  return -1;
}

SnesConfigBarAction snes_config_bar_click(int x, int y) {
  int i, bw;
  if (!snes_config_bar_contains(x, y)) return kCfgAct_None;

  /* The command row, on screen in both forms. */
  if (y >= ROW_H + 1 && y < ROW_H + 2 + ROW_H) {
    bw = g_layout_w / kButtonCount;
    if (bw <= 0) return kCfgAct_None;
    i = x / bw;
    if (i >= 0 && i < kButtonCount) return kButtons[i].act;
    return kCfgAct_None;
  }
  if (snes_config_bar_row_at(x, y) >= 0) return kCfgAct_Select;
  return kCfgAct_None;
}
