# SNES Mouse (Player 2)

Status: **landed** · off by default · `SNESRECOMP_MOUSE=1`

snesrecomp emulates the SNES Mouse peripheral on a controller port. The
reference for the wire protocol is [bsnes] — the reference emulator — because
no wiki table is authoritative enough for cycle-accurate behaviour and bsnes
is the baseline the rest of the hybrid read paths were already matched to.

The mouse is opt-in (`SNESRECOMP_MOUSE=1`) and plugs into **port 2**, which is
where the original SimCity mouse software expected it. While on, the host OS
pointer is hidden so the ROM's own cursor is the only one on screen.

## 1. Hardware model

A SNES Mouse looks like a controller to the console: it sits on a port's
Data1 line and answers the same strobe-and-shift handshake. What differs:

- **32 data bits** instead of a pad's 16. The extra 16 carry the right/left
  buttons and movement (the first 16 encode button+signature+speed fields).
- **Signature 0001** in bits 12-15. A pad's signature is 0000; a game probing
  the device by reading past bit 15 distinguishes the two.
- **Two speed bits** (bits 10-11). The host's tracker speed, read as a two-bit
  value; bsnes scales the movement read-out by 1.0 / 1.5 / 2.0 for speed 0/1/2.
- **`$4017` still works**: Data2 of a mouse port reads 0 throughout, and reads
  past bit 31 return 1 (a connected device), as a pad returns 1 past bit 15.

Bit layout (bit 0 is the first bit read after the strobe drops):

| bits | content |
|------|---------|
| 0-7  | sync, all 0 |
| 8    | right button |
| 9    | left button |
| 10-11| speed (msb, lsb) |
| 12-14| 0 |
| 15   | 1 (device signature) |
| 16   | sign-y (1 = up) |
| 17-23| abs(y) as 7 bits |
| 24   | sign-x (1 = left) |
| 25-31| abs(x) as 7 bits |

While the strobe is held the mouse stays quiet (0 on both lines) and **cycles
its speed** by 0 → 1 → 2 per read; when the strobe drops, the movement
accumulated since the last read-out is latched, scaled by the current speed,
and shifted out. Movement that the guest never reads is consumed by the latch
exactly like a peripheral's counters — a frozen guest loses nothing and a
mouse can never be "double-read".

## 2. Enablement

    SNESRECOMP_MOUSE=1  <path-to-rom>

A mouse and a Super Multitap on the same port are mutually exclusive: the
later configuration wins, and a tap that turns on unplugs a mouse on that
port. The mouse is inserted after boot, survives guest resets, and rides the
savestate chunk alongside the pads.

The automatic-read registers report the mouse only as its 16-bit device prefix
(`$421A/$421B` for port 2): signature bit0 = 1 identifies the device; the
movement/button fields only exist on the manual `$4017` path, which is where
mouse-using games read it.

## 3. Diagnostics

`SNESRECOMP_PAD_PROBE=1` logs each port's manual-read depth and auto-read
activity every 64 frames, for example:

    [padprobe] frame=704 p0 reads=0 maxshift=0 auto=695 p1 reads=0 maxshift=0 auto=695 p1w=0001

`reads`/`maxshift` cover the manual `$4016`/`$4017` path (a game driving a
mouse shifts past 24 bits, a pad stops at 16); `auto` counts the automatic
`$4218`+ reads per port; `pNw` is the last auto word served on that port's
Data1 line, so `p1w=0001` above shows the mouse's signature bit reaching the
guest every frame. Use it headless to prove a title actually reads the device:

    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    SNESRECOMP_MOUSE=1 SNESRECOMP_PAD_PROBE=1 RUN_FRAMES=600 \
    ./SimCitySNESRecomp "$PWD/SimCity (USA).sfc"

## 4. Determinism

Mouse motion is host input, the same class as pad buttons: the recompiled
core stays deterministic for a given feed and savestate. The savestate chunk
carries the pending/latched mouse state so a mid-shift save resumes exactly.

[bsnes]: https://github.com/bsnes-emu/bsnes