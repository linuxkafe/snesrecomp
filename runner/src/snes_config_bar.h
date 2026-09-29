#ifndef SNES_CONFIG_BAR_H
#define SNES_CONFIG_BAR_H

#include <stdint.h>
#include "snes_overlay_draw.h"

/*
 * The configuration bar: every option config.ini can set, on screen, on F1.
 *
 * This is NOT a settings menu. It is a bar that shows what the current
 * configuration IS, because the alternatives were both worse: reading
 * config.ini means leaving the game, and the launcher means restarting. A user
 * who does not know the keys otherwise has no way to find out that a setting is
 * wrong.
 *
 * F1 opens it and F1 closes it. It is not up by default: it covers 21 of the
 * field's 224 rows, and a player who never changes a setting should not pay
 * for that on every frame. In the full list Up/Down select a row and
 * Left/Right change it; the mouse works on both forms for the command buttons,
 * and on the rows in the full list.
 *
 * The bar knows nothing about HOW anything is applied. Everything that
 * touches a window, a device or a renderer arrives through the hooks the host
 * registers, because those live as statics inside the host and this file must
 * not know their names.
 */

typedef enum {
  kCfgAct_None = 0,
  kCfgAct_ToggleExpand,
  kCfgAct_RowLeft, kCfgAct_RowRight, kCfgAct_RowUp, kCfgAct_RowDown,
  kCfgAct_Select,      /* mouse: activate the row or button under the pointer */
  /* Command buttons, in the order they are drawn. */
  kCfgAct_Pause, kCfgAct_VolDown, kCfgAct_VolUp, kCfgAct_States,
  kCfgAct_Rewind, kCfgAct_Screenshot, kCfgAct_Perf
} SnesConfigBarAction;

/* How an option changes. The bar owns the value stepping; the host owns the
 * effect. `enum_names` non-NULL means the value cycles those strings. */
typedef struct {
  const char *section;
  const char *name;          /* the config.ini key, uppercase for the font */
  int (*get)(void);
  void (*step)(int delta);   /* NULL => restart-only, shown greyed */
  const char *const *enum_names; /* NULL => bool or a bounded integer */
  int enum_count;
  int step_magnitude;        /* for integers: 1, 5, 10 ... */
  int min_value, max_value;
} SnesConfigBarOption;

/* Everything the bar cannot do by itself. All may be NULL; a NULL hook makes
 * the rows that need it read-only rather than crashing. */
typedef struct {
  void (*set_window_scale)(int delta);
  void (*set_fullscreen)(void);
  void (*set_volume)(int delta);
  void (*set_widescreen)(int on);
  void (*set_renderer)(int idx);
  void (*set_shader)(int idx);
  void (*set_display_aspect)(int idx);
  void (*set_frame_blend)(int on);
  void (*set_vsync)(int on);
  void (*set_run_ahead)(int delta);
  void (*set_no_sprite_limits)(int on);
  void (*set_linear_filtering)(int on);
  void (*set_perf_title)(int on);
  void (*set_autosave)(int on);
  void (*set_display_perf)(void);   /* the F-key FPS toggle, not a config key */
  void (*set_pause)(void);
  void (*set_states)(void);
  void (*set_rewind)(void);
  void (*set_screenshot)(void);
} SnesConfigBarHooks;

/* Register the hooks and the two accessors for option VALUES.
 *
 * The bar deliberately does not include config.h: that header pulls in SDL,
 * which would make this module untestable by the standalone harness, and a bar
 * that reached into g_config itself would have to be re-audited every time a
 * key moved. The host passes the current value of option `index` through
 * get_value and stores a new one through set_value; the option TABLE below is
 * the bar's, and the VALUES are the host's. */
/* Optional: a short status string for a cheat row ("NO ADDR" while the
 * address is unlocated). NULL leaves cheat rows showing just their value. */
void snes_config_bar_set_cheat_note(const char *(*note)(int cheat_index));

/* RENDERER is supplied by the host because only the host knows which render
 * drivers SDL actually offers on this machine and build - vulkan among them.
 * A hardcoded list here hid it. `current` is a list index, not the stored
 * string, and `choose` applies one. */
void snes_config_bar_set_renderers(int count,
                                   const char *(*name)(int index),
                                   int (*current)(void),
                                   void (*choose)(int index));

void snes_config_bar_init(const SnesConfigBarHooks *hooks,
                          int (*get_value)(int index),
                          void (*set_value)(int index, int value));

/* Draw the bar into the frame about to be presented. `dst` is 32-bit ARGB at
 * `pitch` BYTES per row, dst_w x dst_h pixels, in FRAME coordinates (the
 * pre-scale guest field), because that is the buffer the presenter hands the
 * host. */
void snes_config_bar_draw(uint8_t *dst, int pitch, int dst_w, int dst_h);

/* F1 toggles the bar's visibility; opening it shows the full list. */
void snes_config_bar_toggle_visible(void);
int  snes_config_bar_visible(void);
void snes_config_bar_toggle_expanded(void);
int  snes_config_bar_expanded(void);

/* Hit-test in FRAME coordinates. Returns kCfgAct_None for a miss. */
SnesConfigBarAction snes_config_bar_click(int frame_x, int frame_y);

/* Row hit-testing and editing, so the host can drive the list from its own
 * nav input without duplicating the layout maths. Return -1 for a miss. */
int snes_config_bar_row_at(int frame_x, int frame_y);
int snes_config_bar_select_row(int option_index);
int snes_config_bar_step_row(int option_index, int dir);  /* applies at once */
int snes_config_bar_move(int dir);   /* -1/+1, returns the new selection */
int snes_config_bar_selected(void);
/* A row with no stepper is restart-only: it draws greyed with RESTART and
 * refuses to change. Exposed so a test can assert the split without parsing
 * the drawn text. */
int snes_config_bar_row_editable(int option_index);
int snes_config_bar_row_count(void);

/* Is a point over the bar at all? The host uses this to decide whether a
 * click belongs to the bar or to the guest. */
int snes_config_bar_contains(int frame_x, int frame_y);

#endif
