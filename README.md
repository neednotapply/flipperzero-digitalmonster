# Digital Monster for Flipper Zero

An emulator for the original 1997 Bandai **Digital Monster** virtual pets
(Ver.1, Ver.2 and Ver.3) on the Flipper Zero.

It doesn't re-implement the game. It emulates the pet's actual CPU, an Epson
**E0C6200** 4-bit microcontroller, and runs the original ROM. The CPU core is a
C port of the E0C6200 core from [BrickEmuPy](https://github.com/azya52/BrickEmuPy)
by azya52, so timing, sound and behavior match that emulator.

## Features

- Runs Digimon Ver.1 / Ver.2 / Ver.3 ROM dumps (any `.bin` in the data folder)
- 32x16 dot-matrix LCD scaled 3x, plus the 8 menu icons drawn close to their
  real size in columns either side of it (the toy's top row on the left, its
  bottom row on the right)
- Buzzer sound through the Flipper speaker (Off / Low / High). Stealth mode mutes it.
- Saves automatically on exit and every 5 minutes, one save per ROM
- **Offline time**: when you reopen the app, the pet's clock jumps to the
  current time and the last few minutes you were away are replayed, which takes
  a few seconds at most. Bedtime and waking up still happen on the right side
  of the jump, so you don't come back to a pet that's awake all night or asleep
  all day. For the rest of the gap the pet doesn't age or get hungry. This needs
  the in-game clock to be set. If you set it to a different time than the
  Flipper's, that difference is kept. Set Offline time to Off in the menu to
  freeze the pet and its clock while the app is closed, or press Back to skip
  the replay.
- 1x / 2x / 4x speed, solid or "LCD grid" pixel style

## Installing

1. Copy `digimon.fap` to `SD:/apps/Games/` (or run `ufbt launch`).
2. Copy your ROM dumps to `SD:/apps_data/digimon/`, e.g.
   - `SD:/apps_data/digimon/DigimonV1.bin`
   - `SD:/apps_data/digimon/DigimonV2.bin`
   - `SD:/apps_data/digimon/DigimonV3.bin`

ROMs are **not** included. They're 16 KiB files (8192 12-bit words, stored
big-endian), the same files BrickEmuPy uses. If there's only one ROM, the app
starts it straight away. Otherwise you pick one from a list.

## Controls

| Flipper      | Digimon button       |
|--------------|----------------------|
| Up / Left    | A (top, select)      |
| OK           | B (middle, confirm)  |
| Down / Right | C (bottom, cancel)   |
| Back         | Pause menu           |
| Hold Back    | Save and exit        |

The pause menu has Sound, Speed, LCD style, Offline time, a Controls reminder,
**Reset Digimon** (presses the device's reset pin: the pet is lost and a new egg
appears), and Save & exit.

On first start the pet shows `0:00 AM SET`. Set the clock as on the real toy:
B to start, A to change the value, B to confirm. After that the egg hatches a
few minutes later.

## Building

```
pip install ufbt
ufbt            # build dist/digimon.fap
ufbt launch     # build, upload and run on a connected Flipper
```

CI builds a separate `.fap` for Momentum, Unleashed and official firmware
(release and dev channel of each), downloadable from the workflow run's
artifacts. Use the one that matches your firmware: a `.fap` built for a
different firmware API version may refuse to load.

To build locally for Momentum or Unleashed, point ufbt at their SDK index:

```
ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel=release
ufbt update --index-url=https://up.unleashedflip.com/directory.json --channel=release
```

## Development tools

`tools/` is not part of the Flipper build:

- `host_test.c`: runs the core on a desktop. It can play a scripted button
  sequence and dump screenshots, trace CPU state, benchmark, or check that
  skipping offline time leaves the pet asleep or awake the same way as
  emulating it (`host_test clock <rom.bin>`).
  `gcc -O2 -Isrc tools/host_test.c src/e0c6200.c src/digimon_render.c src/digimon_clock.c -o host_test`
- `ref_trace.py`: prints the same trace format using BrickEmuPy's Python core,
  so you can diff the two implementations.
- `gen_lcd_map.py`: regenerates `src/lcd_map.h` (which VRAM bit drives each LCD
  dot and icon) from a BrickEmuPy face SVG.

## Source layout

| File | Purpose |
|------|---------|
| `src/e0c6200.c/.h` | E0C6200 CPU, timers, ports, buzzer. Platform independent. |
| `src/digimon_render.c/.h` | LCD + icons to a 128x64 XBM framebuffer |
| `src/digimon_clock.c/.h` | Reads, sets and skips ahead the ROM's time-of-day clock |
| `src/lcd_map.h` | Generated VRAM-to-pixel map |
| `src/digimon_app.c` | Flipper app: ROM picker, input, sound, saves, menu |

## Not supported (yet)

- D-Com battle link between devices (the `P2` port is left idle)
- Ver.4 and later have not been tested

## Credits

- E0C6200 emulation and LCD layout: [BrickEmuPy](https://github.com/azya52/BrickEmuPy) by azya52
- Digital Monster is a trademark of Bandai. This project isn't affiliated with Bandai.
