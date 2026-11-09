/*
 * Epson E0C6200 4-bit microcontroller core.
 *
 * C port of the E0C6200 core from BrickEmuPy by azya52
 * (https://github.com/azya52/BrickEmuPy), which is what the original
 * Bandai Digital Monster Ver.1 - Ver.3 virtual pets run on.
 *
 * The core is platform independent (no Flipper headers) so it can be
 * compiled and tested on a desktop machine as well.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define E0C_OSC1_HZ    32768u
#define E0C_RAM_SIZE   0x300
#define E0C_VRAM_SIZE  0x0A0
#define E0C_ROM_WORDS  0x2000 /* 2 banks x 4096 12-bit words */
#define E0C_ROM_BYTES  (E0C_ROM_WORDS * 2)
#define E0C_PORT_COUNT 11

typedef enum {
    E0CPortK0 = 0,
    E0CPortK1,
    E0CPortR0,
    E0CPortR1,
    E0CPortR2,
    E0CPortR3,
    E0CPortR4,
    E0CPortP0,
    E0CPortP1,
    E0CPortP2,
    E0CPortP3,
} E0CPort;

typedef enum {
    E0CLcdBlank = 0, /* LCD off (ALOFF) or CPU held in reset */
    E0CLcdAllOn, /* all segments forced on (ALON) */
    E0CLcdNormal, /* segments come from VRAM */
} E0CLcdMode;

/* Buzzer frequencies in Hz, indexed by BZFQ (CTRL_BZ1 & 7). */
extern const uint16_t e0c6200_buzzer_hz[8];

/*
 * Complete machine state. Contains no pointers, so it can be written to
 * and read from a save file as-is (see E0C6200_STATE_VERSION).
 */
#define E0C6200_STATE_VERSION 1

typedef struct {
    /* CPU registers */
    uint16_t pc; /* 13 bit: bank(1) page(4) step(8) */
    uint16_t npc; /* new page pointer (set by PSET) */
    uint16_t ix; /* 12 bit: XP XH XL */
    uint16_t iy; /* 12 bit: YP YH YL */
    uint8_t sp;
    uint8_t a, b;
    uint8_t cf, zf, df, iflag;
    uint8_t halt;
    uint8_t if_delay;
    uint8_t reset_held;

    uint8_t ram[E0C_RAM_SIZE];
    uint8_t vram[E0C_VRAM_SIZE];

    /* Interrupt factor / mask registers */
    uint8_t it, isw, ipt, isio, ik0, ik1;
    uint8_t eit, eisw, eipt, eisio, eik0, eik1;

    /* Timers */
    uint8_t tm; /* clock timer, 256 Hz */
    uint8_t swl, swh; /* stopwatch, 1/100 s BCD */
    uint8_t pt, rd; /* programmable timer + reload */
    uint8_t sd; /* serial data */
    uint8_t dfk0;

    /* Control registers */
    uint8_t ctrl_osc, ctrl_lcd, lc, ctrl_svd, ctrl_bz1, ctrl_bz2;
    uint8_t ctrl_sw, ctrl_pt, ptc, sc, hzr, ioc, pup;

    /* I/O ports */
    uint8_t pdir[E0C_PORT_COUNT];
    uint8_t pushpull[E0C_PORT_COUNT];
    uint8_t platch[E0C_PORT_COUNT];
    uint8_t pullup[E0C_PORT_COUNT];
    uint8_t pullup_ext[E0C_PORT_COUNT]; /* mask option */
    uint8_t in_low[E0C_PORT_COUNT]; /* pins externally driven low */
    uint8_t in_high[E0C_PORT_COUNT]; /* pins externally driven high */

    /* Clock dividers. osc1_counter is in units where one OSC3 cycle is
     * E0C_OSC1_HZ and one OSC1 cycle is the OSC3 frequency, which keeps
     * the OSC1/OSC3 ratio exact with integer math. */
    int64_t osc1_counter;
    int32_t timer_counter; /* OSC1 ticks */
    int32_t stopwatch_counter; /* 1/100 OSC1 ticks */
    int32_t ptimer_counter; /* OSC1 ticks */
    uint64_t osc1_ticks; /* total OSC1 ticks since power on */

    /* Buzzer */
    int32_t snd_one_shot;
    int32_t snd_env_counter;
    int32_t snd_env_cycle;
    uint8_t snd_freq;
    uint8_t snd_env_step;
    uint8_t snd_env_on;
    uint8_t snd_on;
    /* What the speaker should currently be doing */
    uint8_t audio_on;
    uint8_t audio_freq;
    uint8_t audio_level; /* 0..7 */
    uint8_t reserved;
} E0C6200State;

typedef struct {
    E0C6200State s;
    const uint16_t* rom; /* E0C_ROM_WORDS opcodes */
    uint32_t osc3_hz; /* main clock, 1060000 for Digimon */
    bool fast_halt; /* skip idle time quickly while halted */
} E0C6200;

typedef struct {
    uint8_t pullup_ext[E0C_PORT_COUNT];
} E0C6200Mask;

/* Initialise a fresh machine (power on). rom must stay valid. */
void e0c6200_init(E0C6200* cpu, const uint16_t* rom, uint32_t osc3_hz, const E0C6200Mask* mask);

/* Equivalent of pressing and releasing the RESET pin. */
void e0c6200_reset(E0C6200* cpu);

/* Hold (true) or release (false) the RESET pin. */
void e0c6200_set_reset_pin(E0C6200* cpu, bool held);

/* Drive an input pin: level 0 = low, 1 = high, -1 = released (floating). */
void e0c6200_set_pin(E0C6200* cpu, E0CPort port, uint8_t mask, int level);

/* Execute one instruction (or one idle slot while halted).
 * Returns elapsed main clock cycles. */
uint32_t e0c6200_step(E0C6200* cpu);

/* Run until at least `osc1_ticks` OSC1 ticks (1/32768 s) have elapsed.
 * Returns early if `max_steps` instructions were executed (0 = no limit). */
void e0c6200_run(E0C6200* cpu, uint32_t osc1_ticks, uint32_t max_steps);

E0CLcdMode e0c6200_lcd_mode(const E0C6200* cpu);

/* Read a port as seen from outside the chip. */
uint8_t e0c6200_port_read(const E0C6200* cpu, E0CPort port);
