/*
 * Renders the Digimon LCD (32x16 dots + 8 icons) into a 128x64 1-bit
 * framebuffer in XBM layout (LSB first, 16 bytes per row), which is what
 * canvas_draw_xbm() expects. Platform independent.
 */
#pragma once

#include "e0c6200.h"

#include <stdint.h>

#define DIGIMON_FB_W    128
#define DIGIMON_FB_H    64
#define DIGIMON_FB_SIZE (DIGIMON_FB_W * DIGIMON_FB_H / 8)

/* Where the dot matrix lands on screen (each LCD dot is 3x3 pixels) */
#define DIGIMON_LCD_X     16
#define DIGIMON_LCD_Y     8
#define DIGIMON_LCD_SCALE 3

typedef enum {
    DigimonRenderSolid = 0, /* 3x3 blocks */
    DigimonRenderGrid = 1, /* 2x2 dots with a 1 pixel gap, like the real LCD */
} DigimonRenderStyle;

/* Draws the LCD and icon rows. Clears the whole buffer first. */
void digimon_render(const E0C6200* cpu, uint8_t* fb, DigimonRenderStyle style);

/* Returns a bitmask of lit icons (bit 0 = top-left ... bit 7 = bottom-right). */
uint8_t digimon_icons(const E0C6200* cpu);

/* Index of the "call" (attention) icon in digimon_icons(). */
#define DIGIMON_ICON_CALL 7
