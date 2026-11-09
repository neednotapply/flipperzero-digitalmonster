/*
 * Desktop test harness for the E0C6200 core. Not part of the Flipper build.
 *
 *   host_test trace  <rom.bin> <steps> [interval]   checkpoint trace (diff with ref_trace.py)
 *   host_test render <rom.bin> <script> [prefix]    run a button script, dump screenshots
 *   host_test bench  <rom.bin> <seconds> [fast]     measure emulation speed
 *   host_test clock  <rom.bin>                      check digimon_clock_advance()
 *
 * Build (from repo root):
 *   gcc -O2 -Isrc tools/host_test.c src/e0c6200.c src/digimon_render.c src/digimon_clock.c -o host_test
 *
 * Render script lines: "<seconds> <cmd> [arg]" with cmd one of
 *   A+ A- B+ B- C+ C-   press / release a button
 *   shot <name>         write <prefix><name>.pgm and print the screen as text
 *   fast                enable fast halt skipping
 *   reset               press the reset pin
 */
#include "e0c6200.h"
#include "digimon_clock.h"
#include "digimon_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint16_t rom[E0C_ROM_WORDS];

static void load_rom(const char* path) {
    FILE* f = fopen(path, "rb");
    if(!f) {
        perror(path);
        exit(1);
    }
    static uint8_t buf[E0C_ROM_BYTES];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if(n < 2) exit(1);
    for(size_t i = 0; i < E0C_ROM_WORDS; i++) {
        size_t o = (i * 2) % n;
        rom[i] = ((buf[o] << 8) | buf[(o + 1) % n]) & 0xFFF;
    }
}

static uint32_t fnv(const uint8_t* d, size_t n) {
    uint32_t h = 0x811C9DC5;
    for(size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 0x01000193;
    }
    return h;
}

static void init_cpu(E0C6200* cpu) {
    E0C6200Mask mask = {0};
    mask.pullup_ext[E0CPortK0] = 0xF;
    mask.pullup_ext[E0CPortK1] = 0xF;
    e0c6200_init(cpu, rom, 1060000, &mask);
}

static int trace(long steps, long interval) {
    static E0C6200 cpu;
    init_cpu(&cpu);
    const uint8_t buttons[3] = {4, 2, 1};
    uint32_t lcg = 12345;
    long press_at = 0, release_at = -1;
    int held = 0;
    for(long step = 1; step <= steps; step++) {
        if(step == press_at) {
            lcg = (lcg * 1103515245u + 12345u) & 0x7FFFFFFF;
            held = (lcg >> 16) % 3;
            e0c6200_set_pin(&cpu, E0CPortK0, buttons[held], 0);
            release_at = step + 3000;
        }
        if(step == release_at) {
            e0c6200_set_pin(&cpu, E0CPortK0, buttons[held], -1);
            lcg = (lcg * 1103515245u + 12345u) & 0x7FFFFFFF;
            press_at = step + 20000 + (lcg >> 16) % 40000;
        }
        if(step == 1) press_at = 200000;
        e0c6200_step(&cpu);
        if(step % interval == 0) {
            E0C6200State* s = &cpu.s;
            printf(
                "%ld pc=%04X a=%X b=%X ix=%03X iy=%03X sp=%02X f=%d%d%d%d h=%d tm=%02X lcd=%X "
                "ram=%08X vram=%08X\n",
                step,
                s->pc,
                s->a,
                s->b,
                s->ix,
                s->iy,
                s->sp,
                s->iflag,
                s->df,
                s->zf,
                s->cf,
                s->halt,
                s->tm,
                s->ctrl_lcd,
                (unsigned)fnv(s->ram, sizeof(s->ram)),
                (unsigned)fnv(s->vram, sizeof(s->vram)));
        }
    }
    return 0;
}

static void write_pgm(const char* path, const uint8_t* fb) {
    const int S = 4; /* upscale for viewing */
    FILE* f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "P5\n%d %d\n255\n", DIGIMON_FB_W * S, DIGIMON_FB_H * S);
    for(int y = 0; y < DIGIMON_FB_H * S; y++)
        for(int x = 0; x < DIGIMON_FB_W * S; x++) {
            int px = x / S, py = y / S;
            int on = (fb[py * 16 + px / 8] >> (px % 8)) & 1;
            fputc(on ? 0x20 : 0xF0, f);
        }
    fclose(f);
}

static void print_screen(const uint8_t* fb) {
    for(int y = 0; y < DIGIMON_FB_H; y += 2) {
        for(int x = 0; x < DIGIMON_FB_W; x++) {
            int a = (fb[y * 16 + x / 8] >> (x % 8)) & 1;
            int b = (fb[(y + 1) * 16 + x / 8] >> (x % 8)) & 1;
            putchar(a && b ? '#' : a ? '"' : b ? ',' : ' ');
        }
        putchar('\n');
    }
}

static int render(const char* script_path, const char* prefix) {
    static E0C6200 cpu;
    init_cpu(&cpu);
    FILE* f = fopen(script_path, "r");
    if(!f) {
        perror(script_path);
        return 1;
    }
    char line[256];
    double now = 0;
    static uint8_t fb[DIGIMON_FB_SIZE];
    while(fgets(line, sizeof(line), f)) {
        double t;
        char cmd[64] = {0}, arg[128] = {0};
        if(line[0] == '#' || sscanf(line, "%lf %63s %127s", &t, cmd, arg) < 2) continue;
        if(t > now) {
            e0c6200_run(&cpu, (uint32_t)((t - now) * E0C_OSC1_HZ), 0);
            now = t;
        }
        if((cmd[0] == 'A' || cmd[0] == 'B' || cmd[0] == 'C') && (cmd[1] == '+' || cmd[1] == '-')) {
            int m = cmd[0] == 'A' ? 4 : cmd[0] == 'B' ? 2 : 1;
            e0c6200_set_pin(&cpu, E0CPortK0, m, cmd[1] == '+' ? 0 : -1);
        } else if(!strcmp(cmd, "shot")) {
            digimon_render(&cpu, fb, DigimonRenderSolid);
            char path[512];
            snprintf(path, sizeof(path), "%s%s.pgm", prefix, arg);
            write_pgm(path, fb);
            printf("== %s @ %.2fs pc=%04X osc=%X\n", arg, now, cpu.s.pc, cpu.s.ctrl_osc);
            print_screen(fb);
        } else if(!strcmp(cmd, "fast")) {
            cpu.fast_halt = true;
        } else if(!strcmp(cmd, "reset")) {
            e0c6200_reset(&cpu);
        }
    }
    fclose(f);
    return 0;
}

static int bench(double seconds, int fast) {
    static E0C6200 cpu;
    init_cpu(&cpu);
    cpu.fast_halt = fast != 0;
    uint64_t target = (uint64_t)(seconds * E0C_OSC1_HZ);
    uint64_t steps = 0, osc3 = 0, halted = 0;
    clock_t c0 = clock();
    /* run in 10 ms slices like the app does */
    while(cpu.s.osc1_ticks < target) {
        uint64_t before = cpu.s.osc1_ticks;
        while(cpu.s.osc1_ticks < before + 328) {
            if(cpu.fast_halt && cpu.s.halt) {
                e0c6200_run(&cpu, (uint32_t)(before + 328 - cpu.s.osc1_ticks), 1);
            } else {
                if(cpu.s.halt) halted++;
                if(cpu.s.ctrl_osc & 8) osc3++;
                e0c6200_step(&cpu);
            }
            steps++;
        }
    }
    double el = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf(
        "emulated %.0f s: %llu steps (%llu halted, %llu on OSC3) in %.3f s host\n",
        seconds,
        (unsigned long long)steps,
        (unsigned long long)halted,
        (unsigned long long)osc3,
        el);
    return 0;
}

/* Skipping time with digimon_clock_advance() must leave the pet in the same
 * bedtime / wake-up state as emulating that time. The nibbles compared flip
 * when the pet goes to bed and wakes up (found by diffing RAM across a day,
 * same addresses in Ver.1 - Ver.3): 0x005 is 4 and 0x065 is 7 while asleep,
 * 0x006 / 0x00D step through the going-to-bed sequence (a plain clock jump
 * past bedtime leaves them half way). Other pet state legitimately differs,
 * since a skip does not age the pet. */
static const uint16_t sleep_nibbles[] = {0x005, 0x006, 0x00D, 0x065};

static void run_seconds(E0C6200* cpu, uint32_t s) {
    e0c6200_run(cpu, s * E0C_OSC1_HZ, 0);
}

static void tap(E0C6200* cpu, int mask) {
    e0c6200_set_pin(cpu, E0CPortK0, mask, 0);
    e0c6200_run(cpu, E0C_OSC1_HZ / 5, 0);
    e0c6200_set_pin(cpu, E0CPortK0, mask, -1);
    e0c6200_run(cpu, E0C_OSC1_HZ * 4 / 5, 0);
}

static void run_to(E0C6200* cpu, uint32_t sec_of_day) {
    uint32_t t;
    while(digimon_clock_get(cpu, &t) && t != sec_of_day)
        e0c6200_run(cpu, E0C_OSC1_HZ / 2, 0);
}

static int clock_test(void) {
    static E0C6200 cpu, start, natural;
    init_cpu(&cpu);
    cpu.fast_halt = true;
    run_seconds(&cpu, 8);
    /* B opens the clock, A adds an hour (from 0), C leaves: 2 PM */
    tap(&cpu, 2);
    for(int i = 0; i < 14; i++)
        tap(&cpu, 4);
    tap(&cpu, 1);

    const uint32_t H = 3600, replay = 60;
    const struct {
        uint32_t from, to;
    } cases[] = {
        {18 * H + 30 * 60, 20 * H + 30 * 60}, /* across bedtime (19:00 or 20:00) */
        {21 * H, 23 * H}, /* asleep the whole time */
        {3 * H, 9 * H}, /* across waking up (08:00) */
        {23 * H + 59 * 60 + 59, 1 * H}, /* starts 1 s before midnight */
    };
    int fails = 0;
    for(size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        run_to(&cpu, cases[c].from);
        start = cpu;
        run_to(&cpu, cases[c].to);
        natural = cpu;

        cpu = start;
        uint32_t span = (cases[c].to + DIGIMON_DAY_S - cases[c].from) % DIGIMON_DAY_S;
        digimon_clock_advance(&cpu, span - replay);
        run_seconds(&cpu, replay);

        uint32_t t = 0;
        digimon_clock_get(&cpu, &t);
        int32_t dt = (int32_t)t - (int32_t)cases[c].to;
        bool ok = dt >= -2 && dt <= 2;
        printf(
            "%02u:%02u -> %02u:%02u  clock %02u:%02u:%02u",
            cases[c].from / H,
            cases[c].from / 60 % 60,
            cases[c].to / H,
            cases[c].to / 60 % 60,
            t / H,
            t / 60 % 60,
            t % 60);
        for(size_t i = 0; i < sizeof(sleep_nibbles) / sizeof(sleep_nibbles[0]); i++) {
            uint16_t a = sleep_nibbles[i];
            printf("  %03X=%X/%X", a, cpu.s.ram[a], natural.s.ram[a]);
            if(cpu.s.ram[a] != natural.s.ram[a]) ok = false;
        }
        printf("  %s\n", ok ? "ok" : "MISMATCH");
        if(!ok) fails++;
        cpu = natural;
    }

    /* a gap of several days only needs the clock to land in the right place */
    cpu = natural;
    uint32_t t0 = 0, t1 = 0;
    digimon_clock_get(&cpu, &t0);
    digimon_clock_advance(&cpu, 3 * DIGIMON_DAY_S + 5 * H + 7);
    digimon_clock_get(&cpu, &t1);
    uint32_t want = (t0 + 5 * H + 7) % DIGIMON_DAY_S;
    bool ok = t1 == want;
    printf(
        "3 days + 5 h: clock %02u:%02u:%02u, want %02u:%02u:%02u  %s\n",
        t1 / H,
        t1 / 60 % 60,
        t1 % 60,
        want / H,
        want / 60 % 60,
        want % 60,
        ok ? "ok" : "MISMATCH");
    if(!ok) fails++;
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if(argc >= 3 && !strcmp(argv[1], "clock")) {
        load_rom(argv[2]);
        return clock_test();
    }
    if(argc < 4) {
        fprintf(stderr, "usage: host_test trace|render|bench <rom.bin> ...\n");
        return 1;
    }
    load_rom(argv[2]);
    if(!strcmp(argv[1], "trace")) return trace(atol(argv[3]), argc > 4 ? atol(argv[4]) : 10000);
    if(!strcmp(argv[1], "render")) return render(argv[3], argc > 4 ? argv[4] : "shot_");
    if(!strcmp(argv[1], "bench")) return bench(atof(argv[3]), argc > 4 ? atoi(argv[4]) : 0);
    return 1;
}
