/*
 * Soft mouse: host PC pointer -> the guest's own cursor, via virtual d-pad.
 *
 * ROM-free by construction. joypad_soft_mouse_map touches no SNES state and
 * reads no globals - the host owns the SoftMouseState and passes it in - so
 * this links against joypad.c alone, exactly like mouse_joypad_test.
 *
 * What is pinned here is the behaviour that is easy to get wrong and hard to
 * see on screen:
 *   - a slow drag still registers (sub-threshold motion accumulates);
 *   - motion is applied to the AXIS, not straight to a direction, so a
 *     diagonal drag does not bias one way;
 *   - a pointer warp saturates instead of wrapping (a wrapped accumulator
 *     would flick the cursor backwards);
 *   - a d-pad pulse holds for pulse_frames, and a new direction is not
 *     started until it drains (two directions in one word cancel);
 *   - a click is a rising edge, and a click that lands DURING a pulse is
 *     neither lost nor re-fired on the next frame;
 *   - holding a button does not repeat.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "joypad.h"

#define SM_B 0x0001u
#define SM_UP 0x0010u
#define SM_DOWN 0x0020u
#define SM_LEFT 0x0040u
#define SM_RIGHT 0x0080u
#define SM_A 0x0100u

static int failures;

#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      ++failures;                                                             \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);                           \
      printf(__VA_ARGS__);                                                    \
      printf("\n");                                                           \
    }                                                                         \
  } while (0)

/* Feed `n` frames of constant motion; return the total bits emitted and leave
 * the last frame's bits in *last. */
static int feed(SoftMouseState *st, int dx, int dy, int left, int right,
                int frames, int threshold, int pulse, uint16_t *last) {
  int n = 0;
  for (int i = 0; i < frames; i++) {
    uint16_t bits = 0;
    if (joypad_soft_mouse_map(st, dx, dy, left, right, threshold, pulse, &bits))
      n++;
    *last = bits;
  }
  return n;
}

static void test_slow_drag_accumulates(void) {
  printf("a slow drag still moves the cursor\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  /* One pixel per frame with a threshold of 4 must fire once every 4 frames,
   * not never (dropping the motion) and not every frame (raw dx). */
  int fired = feed(&st, 1, 0, 0, 0, 40, 4, 1, &bits);
  CHECK(fired == 10, "expected 10 pulses over 40 frames of 1px, got %d", fired);
  CHECK(bits == SM_RIGHT, "last pulse should be RIGHT, got %04X", bits);
}

static void test_no_motion_no_pulse(void) {
  printf("a still pointer emits nothing\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0xffff;
  int fired = feed(&st, 0, 0, 0, 0, 30, 4, 1, &bits);
  CHECK(fired == 0, "a still pointer fired %d times", fired);
  CHECK(bits == 0, "bits should be 0, got %04X", bits);
}

static void test_directions(void) {
  printf("each direction maps to its own pad bit\n");
  struct { int dx, dy; uint16_t want; const char *name; } cases[] = {
    {  20,   0, SM_RIGHT, "right" },
    { -20,   0, SM_LEFT,  "left"  },
    {   0,  20, SM_UP,    "up"    },
    {   0, -20, SM_DOWN,  "down"  },
  };
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    SoftMouseState st;
    memset(&st, 0, sizeof st);
    uint16_t bits = 0;
    feed(&st, cases[i].dx, cases[i].dy, 0, 0, 1, 4, 1, &bits);
    CHECK((bits & 0x00F0u) == cases[i].want,
          "%s: expected %04X, got %04X", cases[i].name, cases[i].want, bits);
  }
}

static void test_pulse_holds_and_drains(void) {
  printf("a pulse holds for pulse_frames and blocks the next direction\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  /* Exactly one step's worth of motion (threshold 4, so 4px): with
   * pulse_frames = 3 the guest must see RIGHT on 3 consecutive frames and
   * then nothing, because the accumulator is empty and no new motion
   * arrives. Firing more here would mean the pulse outran the motion. */
  feed(&st, 4, 0, 0, 0, 1, 4, 3, &bits);
  CHECK(bits == SM_RIGHT, "frame 0 should fire RIGHT, got %04X", bits);
  uint16_t seq[6];
  for (int i = 0; i < 6; i++)
    joypad_soft_mouse_map(&st, 0, 0, 0, 0, 4, 3, &seq[i]);
  CHECK(seq[0] == SM_RIGHT && seq[1] == SM_RIGHT,
        "the pulse must stay down for its width, got %04X %04X", seq[0], seq[1]);
  CHECK(seq[2] == 0 && seq[3] == 0 && seq[4] == 0,
        "with no further motion the pulse must drain, got %04X %04X %04X",
        seq[2], seq[3], seq[4]);

  /* Opposite direction arriving mid-pulse must not cancel the pulse: the two
   * directions would share one word and the host cancels both. */
  SoftMouseState st2;
  memset(&st2, 0, sizeof st2);
  uint16_t b2 = 0;
  feed(&st2, 4, 0, 0, 0, 1, 4, 4, &b2);
  joypad_soft_mouse_map(&st2, -4, 0, 0, 0, 4, 4, &b2);
  CHECK(b2 == SM_RIGHT, "mid-pulse motion must not add a second direction, got %04X",
        b2);
}

static void test_warp_saturates(void) {
  printf("a pointer warp saturates instead of wrapping backwards\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  /* 40000px in one frame is a warp (window drag, WM sync). Wrapping a 16-bit
   * accumulator would make the cursor flick the other way. */
  feed(&st, 40000, 0, 0, 0, 1, 4, 1, &bits);
  CHECK(bits == SM_RIGHT, "a warp must read as motion right, got %04X", bits);
  /* The residue must still be positive, so the next frames keep going right. */
  uint16_t b2 = 0;
  joypad_soft_mouse_map(&st, 0, 0, 0, 0, 4, 1, &b2);
  CHECK((b2 & 0x00F0u) == SM_RIGHT || b2 == 0,
        "post-warp direction must not reverse, got %04X", b2);
}

static void test_buttons_are_taps(void) {
  printf("a click is a rising edge and holding does not repeat\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;

  joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 1, &bits);
  CHECK(bits == SM_A, "left click should emit A, got %04X", bits);
  for (int i = 0; i < 30; i++) {
    joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 1, &bits);
    CHECK(bits == 0, "a held button must not repeat, frame %d gave %04X", i, bits);
  }
  joypad_soft_mouse_map(&st, 0, 0, 0, 0, 4, 1, &bits);
  joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 1, &bits);
  CHECK(bits == SM_A, "release then click again must emit A, got %04X", bits);

  SoftMouseState st2;
  memset(&st2, 0, sizeof st2);
  joypad_soft_mouse_map(&st2, 0, 0, 0, 1, 4, 1, &bits);
  CHECK(bits == SM_B, "right click should emit B, got %04X", bits);
}

static void test_click_during_pulse(void) {
  printf("a click during a d-pad pulse is neither lost nor re-fired\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  joypad_soft_mouse_map(&st, 4, 0, 0, 0, 4, 4, &bits);   /* start RIGHT, 4 wide */
  CHECK(bits == SM_RIGHT, "expected RIGHT, got %04X", bits);
  joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 4, &bits);  /* press mid-pulse */
  CHECK((bits & SM_A) && (bits & SM_RIGHT),
        "A and the held direction must coexist, got %04X", bits);
  joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 4, &bits);  /* STILL held */
  CHECK((bits & SM_A) == 0, "a still-held button must not re-fire, got %04X", bits);
  joypad_soft_mouse_map(&st, 0, 0, 0, 0, 4, 4, &bits);  /* release */
  joypad_soft_mouse_map(&st, 0, 0, 1, 0, 4, 4, &bits);  /* press again */
  CHECK((bits & SM_A), "a press after a release must fire again, got %04X", bits);
}

static void test_diagonal_uses_both_axes(void) {
  printf("a diagonal drag uses both axes and neither cancels the other\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  joypad_soft_mouse_map(&st, 20, 20, 0, 0, 4, 1, &bits);
  /* RIGHT|UP in one word is a legal diagonal on a pad and does not cancel. */
  CHECK((bits & SM_RIGHT) && (bits & SM_UP), "expected RIGHT|UP, got %04X", bits);
}

static void test_reset(void) {
  printf("reset drops accumulated motion and any pulse in flight\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0;
  joypad_soft_mouse_map(&st, 3, 0, 0, 0, 4, 4, &bits);  /* 3px banked, pulse up */
  joypad_soft_mouse_map(&st, 1, 0, 0, 0, 4, 4, &bits);  /* 4px total -> fires */
  CHECK(bits == SM_RIGHT, "precondition: expected a pulse, got %04X", bits);
  joypad_soft_mouse_reset(&st);
  joypad_soft_mouse_map(&st, 0, 0, 0, 0, 4, 4, &bits);
  CHECK(bits == 0, "after reset the banked motion and pulse must be gone, got %04X",
        bits);
}

static void test_argument_guards(void) {
  printf("bad arguments are clamped, not obeyed\n");
  SoftMouseState st;
  memset(&st, 0, sizeof st);
  uint16_t bits = 0xffff;
  /* threshold 0 and pulse 0 would divide by zero / never fire if honoured. */
  joypad_soft_mouse_map(&st, 5, 0, 0, 0, 0, 0, &bits);
  CHECK(bits == SM_RIGHT, "threshold 0 must behave as 1, got %04X", bits);
  memset(&st, 0, sizeof st);
  CHECK(joypad_soft_mouse_map(NULL, 1, 1, 0, 0, 1, 1, &bits) == 0,
        "a NULL state must return 0");
  CHECK(joypad_soft_mouse_map(&st, 1, 1, 0, 0, 1, 1, NULL) == 0,
        "a NULL out must return 0");
  joypad_soft_mouse_reset(NULL); /* must not crash */
}

int main(void) {
  test_slow_drag_accumulates();
  test_no_motion_no_pulse();
  test_directions();
  test_pulse_holds_and_drains();
  test_warp_saturates();
  test_buttons_are_taps();
  test_click_during_pulse();
  test_diagonal_uses_both_axes();
  test_reset();
  test_argument_guards();

  if (failures) {
    printf("soft mouse: %d FAILURE(S)\n", failures);
    return 1;
  }
  printf("soft mouse: all checks passed\n");
  return 0;
}
