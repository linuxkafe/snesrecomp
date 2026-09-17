#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "joypad.h"
#include "snes.h"

static int check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
  }
  return 0;
}

/* Strobes port `port`, feeds a fresh latch cycle, and reads 32 bits back,
 * verifying Data1 against `expect` (index 0 = first read) and Data2=0. */
static int shift_out(Snes *snes, unsigned port, const uint8_t *expect,
                     unsigned nbits, const char *tag) {
  int fails = 0;
  unsigned i;
  for (i = 0; i < nbits; i++) {
    uint8_t v = joypad_read_port(snes, port);
    if (v & 2u)
      fails += check(0, "Data2 stays 0 for a mouse");
    if ((v & 1u) != (expect[i] & 1u)) {
      fprintf(stderr, "FAIL: %s bit %u expected %u got %u (line %d)\n",
              tag, i, expect[i] & 1u, v & 1u, __LINE__);
      fails++;
      break;
    }
  }
  return fails;
}

/* The speed field cycles with every read while the strobe is held and then
 * persists, exactly as bsnes does. Bring it deterministically to 0 by cycling
 * `current` -> 0; must run before any motion is fed (the trailing falling
 * edge consumes whatever is pending). */
static void reset_speed(Snes *snes, int current) {
  int need = (3 - current) % 3;
  int i;
  joypad_write_strobe(snes, 1);
  for (i = 0; i < need; i++)
    joypad_read_serial(snes, 1);
  joypad_write_strobe(snes, 0);
}

int main(void) {
  Snes snes;
  int fails = 0;
  static const uint8_t expect_dx5dy3L[32] = {
      0, 0, 0, 0, 0, 0, 0, 0, /* 0-7  sync                            */
      0, 1, 0, 0,             /* 8-11 R, L, speed hi, speed lo        */
      0, 0, 0, 1,             /* 12-15 signature 0001                 */
      1, 0, 0, 0, 0, 0, 1, 1, /* 16-23 sign-y(1=up), |y|=3 as 7 bits   */
      0, 0, 0, 0, 0, 1, 0, 1  /* 24-31 sign-x(0=right), |x|=5 as 7 bits*/
  };
  uint8_t expect[32];
  unsigned i;

  memset(&snes, 0, sizeof(snes));
  joypad_reset_state();

  /* 1. Full protocol stream. dx=5, dy=-3, left held, right clear. */
  joypad_set_device(1, KJOY_DEV_MOUSE);
  fails += check(1 == joypad_get_device(1),
                 "device reported as mouse after set_device(1)");
  joypad_write_strobe(&snes, 0);
  joypad_write_strobe(&snes, 1); /* rising: freeze only */
  fails += check(joypad_set_mouse(1, 5, -3, 1, 0) == 1,
                 "mouse accepts host motion");
  joypad_write_strobe(&snes, 0); /* falling: consume */
  fails += shift_out(&snes, 1, expect_dx5dy3L, 32, "protocol");
  fails += check(joypad_read_serial(&snes, 1) == 1,
                 "reads past bit 31 report a connected device");

  /* 2. High strobe stays quiet and cycles speed; dropping it consumes the
   *    accumulated motion and the cycled speed scales the read-out. */
  fails += check(joypad_set_mouse(1, 10, 0, 0, 0) == 1,
                 "feeds motion for the speed test");
  joypad_write_strobe(&snes, 1);
  fails += check(joypad_read_serial(&snes, 1) == 0,
                 "high strobe reads 0");
  fails += check(joypad_read_serial(&snes, 1) == 0,
                 "high strobe reads 0 again");
  /* two hold-reads cycled speed 0 -> 1 -> 2, so the latch scales x by 2.0 */
  joypad_write_strobe(&snes, 0);
  memset(expect, 0, sizeof(expect));
  expect[10] = (10 >> 1) & 1;   /* speed=2 in the shifted speed field   */
  expect[11] = 10 & 1;          /* speed lsb (2 & 1 = 0)                 */
  expect[15] = 1;               /* signature                             */
  expect[27] = 1;               /* |20| = 0b0010100, bit4 (>>4)          */
  expect[29] = 1;               /* |20| bit2                             */
  fails += shift_out(&snes, 1, expect, 32, "speed-scale");
  /* consumed: a second latch with no new motion shifts zeroes out (the
   * speed field rides over — it only changes while the strobe is held) */
  joypad_write_strobe(&snes, 1);
  joypad_write_strobe(&snes, 0);
  {
    uint8_t ome[32] = {0};
    ome[10] = 1; /* speed=2 persisted from the previous hold */
    ome[15] = 1; /* signature only */
    fails += shift_out(&snes, 1, ome, 32, "consumed");
  }

  /* 3. Clamping at 127 both axes, both directions. */
  reset_speed(&snes, 2); /* land speed on 0 */
  joypad_write_strobe(&snes, 0);
  joypad_write_strobe(&snes, 1);
  fails += check(joypad_set_mouse(1, 200, -200, 0, 0) == 1,
                 "oversized motion accepted");
  joypad_write_strobe(&snes, 0);
  memset(expect, 0, sizeof(expect));
  expect[15] = 1;
  expect[16] = 1;               /* y < 0: up */
  for (i = 17; i <= 23; i++) expect[i] = 1; /* |y| = 127 */
  for (i = 25; i <= 31; i++) expect[i] = 1; /* |x| = 127 */
  fails += shift_out(&snes, 1, expect, 32, "clamp");

  /* 4. Rising edge freezes without consuming. */
  reset_speed(&snes, 0); /* no-op, keeps speed at 0 */
  joypad_write_strobe(&snes, 0);
  joypad_write_strobe(&snes, 1);
  fails += check(joypad_set_mouse(1, 7, 0, 0, 0) == 1, "motion before held strobe");
  joypad_write_strobe(&snes, 1); /* held: relatch (freeze) must not eat it */
  joypad_write_strobe(&snes, 0); /* falling consumes the still-pending 7  */
  memset(expect, 0, sizeof(expect));
  expect[15] = 1;
  expect[29] = 1; /* |7| = 0b111, bit2 */
  expect[30] = 1; /* bit1 */
  expect[31] = 1; /* bit0 */
  fails += shift_out(&snes, 1, expect, 32, "rise-freeze");

  /* 5. Host state survives a guest reset (plugged-in peripherals do). */
  joypad_reset_state();
  fails += check(joypad_get_device(1) == KJOY_DEV_MOUSE,
                 "reset preserves the mouse device");

  /* 6. A pad on port 0 is untouched by a mouse on port 1. */
  snes.input1_currentState = (1u << 0) | (1u << 3) | (1u << 8) | (1u << 11);
  joypad_write_strobe(&snes, 1);
  joypad_write_strobe(&snes, 0);
  for (i = 0; i < 16; i++) {
    uint8_t expected = i == 0 || i == 3 || i == 8 || i == 11;
    fails += check(joypad_read_serial(&snes, 0) == expected,
                   "port 0 pad bit order unaffected");
  }

  /* 7. Automatic read: a mouse port contributes its 16-bit prefix on Data1
   *    ($421A/$421E slots) and Data2 stays 0, without touching pads. */
  joypad_write_strobe(&snes, 0);
  joypad_write_strobe(&snes, 1);
  fails += check(joypad_set_mouse(1, 0, 0, 1, 0) == 1, "motion for auto test");
  joypad_write_strobe(&snes, 0);
  joypad_auto_read(&snes);
  /* mouse idle prefix, left held, speed 0 -> word 0x0041, low byte 0x41 */
  fails += check(joypad_auto_read_reg_addr(&snes, 0x421a) == 0x41,
                 "$421A low byte is the mouse ID prefix");
  fails += check(joypad_auto_read_reg_addr(&snes, 0x421b) == 0x00,
                 "$421A high byte is 0");
  fails += check(joypad_auto_read_reg_addr(&snes, 0x421e) == 0x00,
                 "$421E (Data2 of port 2) is 0 for a mouse");
  /* with no buttons, speed 0 the prefix collapses to bit0=1 (0x0001) */
  joypad_write_strobe(&snes, 1);
  joypad_write_strobe(&snes, 0);
  fails += check(joypad_set_mouse(1, 0, 0, 0, 0) == 1, "clear buttons");
  joypad_auto_read(&snes);
  fails += check(joypad_auto_read_reg_addr(&snes, 0x421a) == 0x01,
                 "idle mouse prefix is the signature alone");

  /* 8. A mouse and a multitap on the same port exclude each other. */
  joypad_set_multitap(1, 1);
  fails += check(joypad_get_device(1) == KJOY_DEV_PAD,
                 "enabling a tap unplugs a mouse on the same port");
  fails += check(joypad_set_mouse(1, 1, 0, 0, 0) == 0,
                 "no mouse, no mouse input accepted");
  joypad_set_device(1, KJOY_DEV_MOUSE);
  fails += check(joypad_get_device(1) == KJOY_DEV_PAD,
                 "mouse refused while a tap owns the port");
  joypad_set_multitap(1, 0);
  joypad_set_device(1, KJOY_DEV_MOUSE);
  fails += check(joypad_get_device(1) == KJOY_DEV_MOUSE,
                 "mouse accepted once the tap is gone");

  if (fails) return 1;
  puts("mouse_joypad_test: PASS");
  return 0;
}