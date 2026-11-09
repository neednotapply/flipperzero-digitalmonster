#include "digimon_render.h"
#include "lcd_map.h"

#include <string.h>

/* Menu icons, redrawn from the printed icons on the Digimon Ver.1-3 LCD
 * (BrickEmuPy face SVG) at close to their real size: on the toy they are
 * about 4 dots tall, which is 12 px at our 3x scale.
 * One string per row, '#' = lit pixel. Width and height can differ per
 * icon (unused rows stay NULL), up to ICON_COL_W wide and ICON_MAX_H tall.
 * Order: top row  status, food, training, battle  (left column)
 *        bottom   flush, lights, medical, call    (right column) */
#define ICON_MAX_H 12

static const char* const icon_art[LCD_ICONS][ICON_MAX_H] = {
    {
        /* status: dial with five graduation marks and a needle */
        "....#######....",
        "..##.......##..",
        ".#.....#.....#.",
        "#....#.#.#....#",
        "#.#..#...#..#.#",
        ".#.#...#...#.#.",
        ".#.....#.....#.",
        ".#.....#.....#.",
        "..#...###...#..",
        "..#.........#..",
        "...#########...",
    },
    {
        /* food: spotted meat with a bone end at each side */
        ".###..##.......",
        ".#.#..#####....",
        "##.##.#.####...",
        "#..#..######...",
        ".##..#####.#...",
        "....####.###...",
        "...##.########.",
        "...#########..#",
        "...####.####.##",
        ".....#####.#.#.",
        ".......##..###.",
    },
    {
        /* training: punching a sandbag */
        ".##....###.",
        ".##....###.",
        "####...###.",
        "###........",
        "###.######.",
        "###....###.",
        "####...###.",
        "####...###.",
        "####...#.#.",
        "####..#..#.",
        "####..#...#",
        ".##....#..#",
    },
    {
        /* battle: two fighters trading punches */
        "...#....#..",
        "..###..###.",
        "..###..###.",
        "...#....#..",
        ".##..#####.",
        ".###...###.",
        ".######.##.",
        ".###...###.",
        ".##...##.#.",
        ".#.#..#..#.",
        "#..#..#...#",
        "#...#.#...#",
    },
    {
        /* flush: poop pile */
        ".....#......",
        ".....##.....",
        "....####....",
        "...#######..",
        "............",
        "..########..",
        "..########..",
        "............",
        "##........##",
        "############",
        ".##########.",
        "...######...",
    },
    {
        /* lights: glowing bulb with a heart filament */
        "......#......",
        "..##.....##..",
        "....#####....",
        "##.#######.##",
        "...#..#..#...",
        "...#.#.#.#...",
        "##.#.###.#.##",
        "...##.#.##...",
        "....##.##....",
        ".##.......##.",
        "....#####....",
        ".....###.....",
    },
    {
        /* medical: crossed bandages */
        "..##....#...",
        ".####..###..",
        "##..####.##.",
        "##..###...##",
        ".#####...##.",
        "..###...##..",
        "..##...###..",
        ".##...#####.",
        "##...###..##",
        ".##.####..##",
        "..###..####.",
        "...#....##..",
    },
    {
        /* call: zaps into a shouting mouth */
        ".......#.......",
        ".......##......",
        "..#....######..",
        ".###...#######.",
        "..###..########",
        "...#.#...######",
        ".....#.....####",
        "..####...######",
        "..###..#######.",
        "##..#....####..",
    },
};

/* The 8 px strips above and below the dot matrix are too small for the
 * icons, so they go in the columns either side of it, one 16 px cell each. */
#define ICON_COL_W  (DIGIMON_LCD_X - 1) /* keep a 1 px gap next to the LCD */
#define ICON_CELL_H (DIGIMON_FB_H / 4)

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
    const char* const* art = icon_art[index];
    int w = (int)strlen(art[0]);
    int h = 0;
    while(h < ICON_MAX_H && art[h])
        h++;
    /* top-row icons on the left, bottom-row icons on the right, centred in their cell */
    int x0 = (index < 4 ? 0 : DIGIMON_FB_W - ICON_COL_W) + (ICON_COL_W - w) / 2;
    int y0 = (index % 4) * ICON_CELL_H + (ICON_CELL_H - h) / 2;
    for(int y = 0; y < h; y++)
        for(int x = 0; art[y][x]; x++)
            if(art[y][x] == '#') set_px(fb, x0 + x, y0 + y);
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
