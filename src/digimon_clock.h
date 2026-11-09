/*
 * Reads and writes the time of day kept by the Digimon Ver.1 - Ver.3 ROMs,
 * so the app can move the toy's clock forward instead of emulating every
 * second the app was closed. Platform independent.
 *
 * All three ROMs keep the clock in RAM as 4-bit digits:
 *   0x01A / 0x01B  seconds (ones / tens)
 *   0x01C / 0x01D  minutes (ones / tens)
 *   0x01E          hour, 1 - 12
 *   0x01F          1 = PM
 *   0x018          bit 3 set until the clock is first set,
 *                  bit 2 set while the clock set screen is open
 */
#pragma once

#include "e0c6200.h"

#include <stdbool.h>
#include <stdint.h>

#define DIGIMON_DAY_S 86400u

/* Enough instructions to finish any game tick (one takes ~1300). */
#define DIGIMON_SETTLE_STEPS 20000u

/* Time of day in seconds since midnight. Returns false if the clock has
 * not been set yet, is being set right now, or the RAM does not look like
 * a Digimon clock (some other ROM). Call e0c6200_settle() first if the CPU
 * may be part way through a game tick. */
bool digimon_clock_get(const E0C6200* cpu, uint32_t* sec_of_day);

/* Sets the time of day (taken modulo one day). Lets the CPU finish the
 * game tick it may be in first, so the game never sees a half-written
 * clock. Returns false and changes nothing if digimon_clock_get() would
 * fail. */
bool digimon_clock_set(E0C6200* cpu, uint32_t sec_of_day);

/* Moves the clock forward by `seconds` without emulating them, so the pet
 * itself does not age. The ROM puts the pet to bed and wakes it up on the
 * hour, and only when it sees that hour arrive, so a plain jump can leave
 * it awake all night or asleep all day. To avoid that, every hour boundary
 * in the last day of the gap is crossed by emulating a few seconds around
 * it. Costs at most ~24 x 4 emulated seconds. Returns false (and does
 * nothing) if the clock is not set. */
bool digimon_clock_advance(E0C6200* cpu, uint32_t seconds);
