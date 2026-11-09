#include "digimon_render.h"
#include "lcd_map.h"

#include <string.h>

/* 8x8 menu icons, MSB = leftmost pixel.
 * Order: top row  status, food, training, battle
 *        bottom   flush, lights, medical, call */
static const uint8_t icon_bits[LCD_ICONS][8] = {
    {0x66, 0xFF, 0xFF, 0xFF, 0x7E, 0x3C, 0x18, 0x00}, /* status: heart */
    {0x3C, 0x7E, 0x7E, 0x7E, 0x3C, 0x18, 0x24, 0x24}, /* food: drumstick */
    {0x00, 0xC3, 0xC3, 0xFF, 0xFF, 0xC3, 0xC3, 0x00}, /* training: dumbbell */
    {0x81, 0x42, 0x24, 0x18, 0x18, 0x66, 0x42, 0x81}, /* battle: crossed swords */
    {0x00, 0x66, 0x99, 0x00, 0x66, 0x99, 0x00, 0xFF}, /* flush: water */
    {0x3C, 0x42, 0x81, 0x81, 0x42, 0x3C, 0x3C, 0x18}, /* lights: bulb */
    {0x3C, 0x24, 0xE7, 0x81, 0x81, 0xE7, 0x24, 0x3C}, /* medical: cross */
    {0x7E, 0x99, 0x99, 0x99, 0x81, 0x99, 0x7E, 0x20}, /* call: "!" bubble */
};

#define ICON_SLOT_W (LCD_W * DIGIMON_LCD_SCALE / 4)
#define ICON_TOP_Y  0
#define ICON_BOT_Y  (DIGIMON_LCD_Y + LCD_H * DIGIMON_LCD_SCALE)

static inline void set_px(uint8_t* fb, int x, int y) {
    fb[y * (DIGIMON_FB_W / 8) + (x >> 3)] |= (uint8_t)(1 << (x & 7));
}

static inline int seg_on(const E0C6200* cpu, E0CLcdMode mode, uint16_t map) {
    if(mode == E0CLcdAllOn) return 1;
    if(mode == E0CLcdBlank) return 0;
    return (cpu->s.vram[map >> 2] >> (map & 3)) & 1;
}

uint8_t digimon_icons(const E0C6200* cpu) {
    E0CLcdMode mode = e0c6200_lcd_mode(cpu);
    uint8_t mask = 0;
    for(int i = 0; i < LCD_ICONS; i++)
        if(seg_on(cpu, mode, lcd_icon_map[i])) mask |= (uint8_t)(1 << i);
    return mask;
}

static void draw_icon(uint8_t* fb, int index) {
    int x0 = DIGIMON_LCD_X + (index % 4) * ICON_SLOT_W + (ICON_SLOT_W - 8) / 2;
    int y0 = index < 4 ? ICON_TOP_Y : ICON_BOT_Y;
    for(int y = 0; y < 8; y++) {
        uint8_t row = icon_bits[index][y];
        for(int x = 0; x < 8; x++)
            if(row & (0x80 >> x)) set_px(fb, x0 + x, y0 + y);
    }
}

void digimon_render(const E0C6200* cpu, uint8_t* fb, DigimonRenderStyle style) {
    memset(fb, 0, DIGIMON_FB_SIZE);
    E0CLcdMode mode = e0c6200_lcd_mode(cpu);
    int dot = style == DigimonRenderGrid ? DIGIMON_LCD_SCALE - 1 : DIGIMON_LCD_SCALE;

    for(int r = 0; r < LCD_H; r++) {
        for(int c = 0; c < LCD_W; c++) {
            if(!seg_on(cpu, mode, lcd_map[r][c])) continue;
            int x0 = DIGIMON_LCD_X + c * DIGIMON_LCD_SCALE;
            int y0 = DIGIMON_LCD_Y + r * DIGIMON_LCD_SCALE;
            for(int y = 0; y < dot; y++)
                for(int x = 0; x < dot; x++)
                    set_px(fb, x0 + x, y0 + y);
        }
    }

    for(int i = 0; i < LCD_ICONS; i++)
        if(seg_on(cpu, mode, lcd_icon_map[i])) draw_icon(fb, i);
}
