/*
 * Digital Monster (Digimon Ver.1 - Ver.3) emulator for Flipper Zero.
 *
 * Runs the original virtual pet ROMs on an emulated Epson E0C6200 CPU
 * (ported from BrickEmuPy). ROMs are not included: copy them to
 * SD:/apps_data/digimon/ (for example DigimonV1.bin).
 *
 * Controls while playing:
 *   Up / Left    -> A (top button: select)
 *   OK           -> B (middle button: confirm)
 *   Down / Right -> C (bottom button: cancel)
 *   Back (tap)   -> C
 *   Back (hold)  -> pause menu (Save & exit is in there)
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <storage/storage.h>

#include <stdlib.h>
#include <string.h>

#include "e0c6200.h"
#include "digimon_clock.h"
#include "digimon_render.h"

#define TAG "Digimon"

#define DATA_DIR      EXT_PATH("apps_data/digimon")
#define SETTINGS_PATH DATA_DIR "/settings.bin"

#define MAX_ROMS     24
#define ROM_NAME_LEN 40
#define OSC3_HZ      1060000

#define SAVE_MAGIC       0x4E4D4744u /* "DGMN" */
#define SETTINGS_MAGIC   0x54534744u /* "DGST" */
#define SETTINGS_VERSION 1
#define AUTOSAVE_MS      (5u * 60u * 1000u)
#define MAX_FRAME_LAG_MS 250u
#define LOOP_PERIOD_MS   10u
/* Offline time: the clock jumps ahead, then the last few minutes are
 * replayed so the pet has lived a little. The replay stops early if it
 * takes longer than CATCHUP_MAX_MS. */
#define REPLAY_MAX_S     (10u * 60u)
#define CATCHUP_MAX_MS   3000u
#define CATCHUP_SLICE_S  1u
#define SPEAKER_TIMEOUT  0u /* never stall emulation waiting for the speaker */

/* Digimon buttons on port K0 */
#define BTN_A 0x4
#define BTN_B 0x2
#define BTN_C 0x1

/* A Back tap presses C for TAP_TICKS of emulated time (the ROM needs ~40 ms
 * to notice a press), then leaves it up for TAP_GAP_TICKS so taps in quick
 * succession still arrive as separate presses. */
#define TAP_TICKS     (E0C_OSC1_HZ / 10u)
#define TAP_GAP_TICKS (E0C_OSC1_HZ / 10u)
#define TAP_QUEUE_MAX 4

typedef enum {
    ScreenRomList,
    ScreenEmu,
    ScreenMenu,
    ScreenCatchup,
    ScreenMessage,
    ScreenConfirmReset,
} Screen;

typedef enum {
    MenuResume,
    MenuSound,
    MenuSpeed,
    MenuLcd,
    MenuCatchup,
    MenuControls,
    MenuReset,
    MenuSaveExit,
    MenuCount,
} MenuItem;

typedef enum {
    SoundOff,
    SoundLow,
    SoundHigh,
    SoundCount,
} SoundLevel;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t sound;
    uint8_t lcd_style;
    uint8_t catch_up;
    uint8_t reserved;
} Settings;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t state_size;
    uint32_t rom_crc;
    uint32_t timestamp;
} SaveHeader;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    Storage* storage;

    Screen screen;
    bool running;

    /* ROM list */
    char roms[MAX_ROMS][ROM_NAME_LEN];
    bool rom_has_save[MAX_ROMS];
    uint8_t rom_count;
    uint8_t rom_index;

    /* Emulation */
    uint16_t* rom;
    uint32_t rom_crc;
    E0C6200* cpu;
    char rom_name[ROM_NAME_LEN];
    uint8_t fb[DIGIMON_FB_SIZE];
    uint8_t last_vram[E0C_VRAM_SIZE];
    uint8_t last_lcd_mode;
    uint32_t last_tick;
    uint32_t tick_rem;
    uint32_t last_save_tick;
    uint8_t keys_down;
    uint8_t buttons;
    uint8_t speed;
    uint8_t taps_queued; /* Back taps waiting to be sent as C presses */
    bool tap_down; /* C is held down for a tap */
    uint64_t tap_until; /* OSC1 tick when the tap press or the gap after it ends */

    /* Menu / messages */
    uint8_t menu_index;
    const char* msg_title;
    const char* msg_text;
    Screen msg_return;

    /* Catch-up */
    uint32_t away_s; /* how long the app was closed */
    uint32_t skip_s; /* clock jump still to do before the replay */
    uint32_t catchup_total; /* seconds to replay */
    uint32_t catchup_done;
    uint32_t catchup_start; /* now_ms() when catching up began */
    bool clock_sync; /* the game clock is set: keep it in step with the RTC */
    uint32_t clock_offset; /* game time of day minus RTC time of day */

    Settings settings;

    /* Sound */
    bool speaker;
    bool tone_on;
    uint8_t tone_freq;
    uint8_t tone_level;
} DigimonApp;

static const uint8_t speeds[] = {1, 2, 4};

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t now_ms(void) {
    /* 64-bit so this doesn't overflow after ~71 minutes; wraps like the tick counter */
    return (uint32_t)((uint64_t)furi_get_tick() * 1000u / furi_kernel_get_tick_frequency());
}

static void rom_path(char* out, size_t len, const char* name, const char* ext) {
    snprintf(out, len, "%s/%s%s", DATA_DIR, name, ext);
}

static void show_message(DigimonApp* app, const char* title, const char* text, Screen ret) {
    app->msg_title = title;
    app->msg_text = text;
    app->msg_return = ret;
    app->screen = ScreenMessage;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

static void settings_load(DigimonApp* app) {
    app->settings = (Settings){
        .magic = SETTINGS_MAGIC,
        .version = SETTINGS_VERSION,
        .sound = SoundHigh,
        .lcd_style = DigimonRenderSolid,
        .catch_up = 1,
    };
    File* file = storage_file_alloc(app->storage);
    Settings s;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &s, sizeof(s)) == sizeof(s) && s.magic == SETTINGS_MAGIC &&
       s.version == SETTINGS_VERSION) {
        if(s.sound < SoundCount) app->settings.sound = s.sound;
        app->settings.lcd_style = s.lcd_style ? DigimonRenderGrid : DigimonRenderSolid;
        app->settings.catch_up = s.catch_up ? 1 : 0;
    }
    storage_file_close(file);
    storage_file_free(file);
}

static void settings_save(DigimonApp* app) {
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &app->settings, sizeof(app->settings));
    }
    storage_file_close(file);
    storage_file_free(file);
}

/* ------------------------------------------------------------------ */
/* ROM list                                                            */
/* ------------------------------------------------------------------ */

static bool has_bin_ext(const char* name) {
    size_t n = strlen(name);
    if(n < 5) return false;
    const char* e = name + n - 4;
    return e[0] == '.' && (e[1] == 'b' || e[1] == 'B') && (e[2] == 'i' || e[2] == 'I') &&
           (e[3] == 'n' || e[3] == 'N');
}

static void scan_roms(DigimonApp* app) {
    app->rom_count = 0;
    File* dir = storage_file_alloc(app->storage);
    if(storage_dir_open(dir, DATA_DIR)) {
        FileInfo info;
        char name[64];
        while(app->rom_count < MAX_ROMS && storage_dir_read(dir, &info, name, sizeof(name))) {
            if(file_info_is_dir(&info) || !has_bin_ext(name)) continue;
            if(info.size < 2 || info.size > E0C_ROM_BYTES) continue;
            name[strlen(name) - 4] = '\0';
            if(strlen(name) >= ROM_NAME_LEN) continue;
            strcpy(app->roms[app->rom_count++], name);
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);

    /* sort by name */
    for(int i = 1; i < app->rom_count; i++) {
        for(int j = i; j > 0 && strcmp(app->roms[j - 1], app->roms[j]) > 0; j--) {
            char tmp[ROM_NAME_LEN];
            strcpy(tmp, app->roms[j]);
            strcpy(app->roms[j], app->roms[j - 1]);
            strcpy(app->roms[j - 1], tmp);
        }
    }

    char path[128];
    for(int i = 0; i < app->rom_count; i++) {
        rom_path(path, sizeof(path), app->roms[i], ".sav");
        app->rom_has_save[i] = storage_common_stat(app->storage, path, NULL) == FSE_OK;
    }
    if(app->rom_index >= app->rom_count) app->rom_index = 0;
}

/* ------------------------------------------------------------------ */
/* Sound                                                               */
/* ------------------------------------------------------------------ */

static bool sound_allowed(DigimonApp* app) {
    return app->settings.sound != SoundOff && !furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode);
}

static void sound_stop(DigimonApp* app) {
    if(app->speaker && app->tone_on) furi_hal_speaker_stop();
    app->tone_on = false;
}

static void sound_release(DigimonApp* app) {
    sound_stop(app);
    if(app->speaker) {
        furi_hal_speaker_release();
        app->speaker = false;
    }
}

static void sound_update(DigimonApp* app) {
    const E0C6200State* s = &app->cpu->s;
    bool want = s->audio_on && sound_allowed(app) && app->screen != ScreenCatchup;
    if(!want) {
        sound_stop(app);
        return;
    }
    if(!app->speaker) {
        app->speaker = furi_hal_speaker_acquire(SPEAKER_TIMEOUT);
        if(!app->speaker) return;
    }
    if(app->tone_on && app->tone_freq == s->audio_freq && app->tone_level == s->audio_level)
        return;
    float volume = app->settings.sound == SoundLow ? 0.15f : 0.8f;
    volume *= 0.4f + 0.6f * (float)s->audio_level / 7.0f;
    furi_hal_speaker_start((float)e0c6200_buzzer_hz[s->audio_freq & 7], volume);
    app->tone_on = true;
    app->tone_freq = s->audio_freq;
    app->tone_level = s->audio_level;
}

/* ------------------------------------------------------------------ */
/* Emulation                                                           */
/* ------------------------------------------------------------------ */

static void set_buttons(DigimonApp* app, uint8_t buttons) {
    uint8_t changed = app->buttons ^ buttons;
    for(uint8_t bit = 1; bit <= BTN_A; bit <<= 1) {
        if(changed & bit) e0c6200_set_pin(app->cpu, E0CPortK0, bit, (buttons & bit) ? 0 : -1);
    }
    app->buttons = buttons;
}

static uint8_t key_button(InputKey key) {
    switch(key) {
    case InputKeyUp:
    case InputKeyLeft:
        return BTN_A;
    case InputKeyOk:
        return BTN_B;
    case InputKeyDown:
    case InputKeyRight:
        return BTN_C;
    default:
        return 0;
    }
}

static void release_all_keys(DigimonApp* app) {
    app->keys_down = 0;
    app->taps_queued = 0;
    app->tap_down = false;
    app->tap_until = 0;
    if(app->cpu) set_buttons(app, 0);
}

static void update_buttons_from_keys(DigimonApp* app) {
    uint8_t buttons = 0;
    for(int k = 0; k < InputKeyMAX; k++)
        if(app->keys_down & (1 << k)) buttons |= key_button((InputKey)k);
    if(app->tap_down) buttons |= BTN_C;
    set_buttons(app, buttons);
}

/* Steps a Back tap along: press C, hold it, let it go, wait, next tap. */
static void tap_service(DigimonApp* app) {
    uint64_t now = app->cpu->s.osc1_ticks;
    if(app->tap_until && now < app->tap_until) return;
    if(app->tap_down) {
        app->tap_down = false;
        app->tap_until = now + TAP_GAP_TICKS;
    } else if(app->taps_queued) {
        app->taps_queued--;
        app->tap_down = true;
        app->tap_until = now + TAP_TICKS;
    } else {
        app->tap_until = 0;
    }
    update_buttons_from_keys(app);
}

/* Re-render the framebuffer if the LCD changed. Returns true if it did. */
static bool refresh_frame(DigimonApp* app, bool force) {
    const E0C6200State* s = &app->cpu->s;
    uint8_t mode = (uint8_t)e0c6200_lcd_mode(app->cpu);
    if(!force && mode == app->last_lcd_mode && memcmp(s->vram, app->last_vram, E0C_VRAM_SIZE) == 0)
        return false;
    memcpy(app->last_vram, s->vram, E0C_VRAM_SIZE);
    app->last_lcd_mode = mode;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    digimon_render(app->cpu, app->fb, (DigimonRenderStyle)app->settings.lcd_style);
    furi_mutex_release(app->mutex);
    return true;
}

static bool save_game(DigimonApp* app) {
    if(!app->cpu) return false;
    char path[128];
    rom_path(path, sizeof(path), app->rom_name, ".sav");
    SaveHeader hdr = {
        .magic = SAVE_MAGIC,
        .version = E0C6200_STATE_VERSION,
        .state_size = sizeof(E0C6200State),
        .rom_crc = app->rom_crc,
        .timestamp = furi_hal_rtc_get_timestamp(),
    };
    bool ok = false;
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        ok = storage_file_write(file, &hdr, sizeof(hdr)) == sizeof(hdr) &&
             storage_file_write(file, &app->cpu->s, sizeof(E0C6200State)) == sizeof(E0C6200State);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) FURI_LOG_E(TAG, "save failed: %s", path);
    app->last_save_tick = now_ms();
    return ok;
}

/* Restores the save, if there is a valid one, and returns the RTC time it was written. */
static bool load_game(DigimonApp* app, uint32_t* saved_at) {
    char path[128];
    rom_path(path, sizeof(path), app->rom_name, ".sav");
    bool loaded = false;
    SaveHeader hdr;
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == SAVE_MAGIC &&
       hdr.version == E0C6200_STATE_VERSION && hdr.state_size == sizeof(E0C6200State) &&
       hdr.rom_crc == app->rom_crc) {
        E0C6200State* state = malloc(sizeof(E0C6200State));
        if(storage_file_read(file, state, sizeof(E0C6200State)) == sizeof(E0C6200State)) {
            app->cpu->s = *state;
            /* buttons may have been held at autosave time: start released */
            app->cpu->s.in_low[E0CPortK0] = 0;
            app->cpu->s.in_high[E0CPortK0] = 0;
            *saved_at = hdr.timestamp;
            loaded = true;
        }
        free(state);
    }
    storage_file_close(file);
    storage_file_free(file);
    return loaded;
}

static bool load_rom(DigimonApp* app, const char* name) {
    char path[128];
    rom_path(path, sizeof(path), name, ".bin");
    uint8_t* bytes = malloc(E0C_ROM_BYTES);
    size_t n = 0;
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        n = storage_file_read(file, bytes, E0C_ROM_BYTES);
    }
    storage_file_close(file);
    storage_file_free(file);
    if(n < 2) {
        free(bytes);
        return false;
    }
    app->rom_crc = crc32(bytes, n);
    app->rom = malloc(E0C_ROM_WORDS * sizeof(uint16_t));
    /* 12-bit opcodes stored as big endian 16-bit words; smaller ROMs mirror */
    for(size_t i = 0; i < E0C_ROM_WORDS; i++) {
        size_t o = (i * 2) % n;
        app->rom[i] = (uint16_t)(((bytes[o] << 8) | bytes[(o + 1) % n]) & 0xFFF);
    }
    free(bytes);
    return true;
}

static void stop_game(DigimonApp* app, bool save) {
    if(!app->cpu) return;
    release_all_keys(app);
    if(save) save_game(app);
    sound_release(app);
    free(app->cpu);
    app->cpu = NULL;
    free(app->rom);
    app->rom = NULL;
}

static void start_game(DigimonApp* app, uint8_t index) {
    strcpy(app->rom_name, app->roms[index]);
    if(!load_rom(app, app->rom_name)) {
        show_message(app, "Error", "Could not read ROM file", ScreenRomList);
        return;
    }
    app->cpu = malloc(sizeof(E0C6200));
    E0C6200Mask mask = {0};
    mask.pullup_ext[E0CPortK0] = 0xF;
    mask.pullup_ext[E0CPortK1] = 0xF;
    e0c6200_init(app->cpu, app->rom, OSC3_HZ, &mask);
    app->cpu->fast_halt = true;

    uint32_t saved_at = 0;
    bool loaded = load_game(app, &saved_at);
    uint32_t now = furi_hal_rtc_get_timestamp();
    uint32_t away = loaded && now > saved_at ? now - saved_at : 0;
    app->keys_down = 0;
    app->buttons = 0;
    app->taps_queued = 0;
    app->tap_down = false;
    app->tap_until = 0;
    app->speed = 1;
    app->tick_rem = 0;
    app->last_tick = now_ms();
    app->last_save_tick = app->last_tick;
    app->tone_on = false;
    refresh_frame(app, true);

    if(away > 2 && app->settings.catch_up) {
        /* Remember how far the game clock was from the RTC when saved, so
         * a clock the player set to some other time stays that way. The
         * save may have been written mid tick, so finish that tick first. */
        uint32_t game;
        e0c6200_settle(app->cpu, DIGIMON_SETTLE_STEPS);
        app->clock_sync = digimon_clock_get(app->cpu, &game);
        if(app->clock_sync)
            app->clock_offset = (game + DIGIMON_DAY_S - saved_at % DIGIMON_DAY_S) % DIGIMON_DAY_S;
        app->away_s = away;
        app->catchup_total = away < REPLAY_MAX_S ? away : REPLAY_MAX_S;
        app->skip_s = app->clock_sync ? away - app->catchup_total : 0;
        app->catchup_done = 0;
        app->catchup_start = now_ms();
        app->screen = ScreenCatchup;
    } else {
        app->screen = ScreenEmu;
    }
}

static void emu_tick(DigimonApp* app) {
    uint32_t now = now_ms();
    uint32_t dt = now - app->last_tick;
    app->last_tick = now;
    if(dt > MAX_FRAME_LAG_MS) dt = MAX_FRAME_LAG_MS; /* drop time we can't keep up with */

    uint64_t t = (uint64_t)dt * E0C_OSC1_HZ * app->speed + app->tick_rem;
    uint32_t ticks = (uint32_t)(t / 1000u);
    app->tick_rem = (uint32_t)(t % 1000u);
    if(ticks) e0c6200_run(app->cpu, ticks, 0);
    if(app->tap_until || app->taps_queued) tap_service(app);

    sound_update(app);
    if(refresh_frame(app, false)) view_port_update(app->view_port);

    if(now - app->last_save_tick > AUTOSAVE_MS) save_game(app);
}

/* Ends catching up (done, out of time, or skipped): moves the game clock
 * to the current time and resumes normal play. */
static void catchup_finish(DigimonApp* app) {
    if(app->clock_sync) {
        uint32_t game, now = furi_hal_rtc_get_timestamp();
        uint32_t target = (now % DIGIMON_DAY_S + app->clock_offset) % DIGIMON_DAY_S;
        if(digimon_clock_get(app->cpu, &game)) {
            uint32_t ahead = (target + DIGIMON_DAY_S - game) % DIGIMON_DAY_S;
            /* a game clock a second or two fast is left alone */
            if(ahead <= DIGIMON_DAY_S / 2) digimon_clock_advance(app->cpu, ahead);
        }
    }
    app->skip_s = 0;
    app->screen = ScreenEmu;
    app->last_tick = now_ms();
    app->tick_rem = 0;
    refresh_frame(app, true);
    view_port_update(app->view_port);
}

/* Deals with the time the Flipper spent outside the app: jump the clock to
 * a few minutes ago, then replay those minutes. */
static void catchup_tick(DigimonApp* app) {
    if(app->skip_s) {
        digimon_clock_advance(app->cpu, app->skip_s);
        app->skip_s = 0;
    }
    uint32_t start = now_ms();
    while(app->catchup_done < app->catchup_total && now_ms() - start < 100) {
        uint32_t slice = app->catchup_total - app->catchup_done;
        if(slice > CATCHUP_SLICE_S) slice = CATCHUP_SLICE_S;
        e0c6200_run(app->cpu, slice * E0C_OSC1_HZ, 0);
        app->catchup_done += slice;
    }
    if(app->catchup_done >= app->catchup_total || now_ms() - app->catchup_start >= CATCHUP_MAX_MS)
        catchup_finish(app);
    else
        view_port_update(app->view_port);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void draw_rom_list(Canvas* canvas, DigimonApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Digital Monster");
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);
    if(app->rom_count == 0) {
        elements_multiline_text_aligned(
            canvas,
            64,
            16,
            AlignCenter,
            AlignTop,
            "No ROMs found.\nCopy DigimonV1.bin etc. to\nSD/apps_data/digimon/");
        return;
    }

    const int visible = 4;
    int first = app->rom_index - visible + 1;
    if(first < 0) first = 0;
    for(int i = 0; i < visible && first + i < app->rom_count; i++) {
        int idx = first + i;
        int y = 15 + i * 12;
        if(idx == app->rom_index) {
            canvas_draw_rbox(canvas, 0, y, 122, 12, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 4, y + 9, app->roms[idx]);
        if(app->rom_has_save[idx])
            canvas_draw_str_aligned(canvas, 118, y + 9, AlignRight, AlignBottom, "saved");
        canvas_set_color(canvas, ColorBlack);
    }
    elements_scrollbar_pos(canvas, 127, 14, 50, app->rom_index, app->rom_count);
}

static void draw_emu(Canvas* canvas, DigimonApp* app) {
    canvas_draw_xbm(canvas, 0, 0, DIGIMON_FB_W, DIGIMON_FB_H, app->fb);
    /* status marks go in the free strips above / below the dot matrix,
     * clear of the icon columns */
    const int x = DIGIMON_LCD_X;
    canvas_set_font(canvas, FontKeyboard);
    if(app->speed > 1) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%ux", app->speed);
        canvas_draw_str(canvas, x, 63, buf);
    }
    if(!sound_allowed(app)) {
        /* small crossed speaker */
        canvas_draw_box(canvas, x + 1, 2, 2, 4);
        canvas_draw_line(canvas, x + 3, 2, x + 5, 0);
        canvas_draw_line(canvas, x + 3, 5, x + 5, 7);
        canvas_draw_line(canvas, x + 5, 0, x + 5, 7);
        canvas_draw_line(canvas, x + 7, 2, x + 10, 5);
        canvas_draw_line(canvas, x + 10, 2, x + 7, 5);
    }
}

static void menu_item_text(DigimonApp* app, int item, char* out, size_t len) {
    static const char* sound_names[] = {"Off", "Low", "High"};
    switch(item) {
    case MenuResume:
        snprintf(out, len, "Resume");
        break;
    case MenuSound:
        snprintf(out, len, "Sound: < %s >", sound_names[app->settings.sound]);
        break;
    case MenuSpeed:
        snprintf(out, len, "Speed: < %ux >", app->speed);
        break;
    case MenuLcd:
        snprintf(out, len, "LCD: < %s >", app->settings.lcd_style ? "Grid" : "Solid");
        break;
    case MenuCatchup:
        snprintf(out, len, "Offline time: < %s >", app->settings.catch_up ? "Sync" : "Off");
        break;
    case MenuControls:
        snprintf(out, len, "Controls");
        break;
    case MenuReset:
        snprintf(out, len, "Reset Digimon");
        break;
    default:
        snprintf(out, len, "Save & exit");
        break;
    }
}

static void draw_menu(Canvas* canvas, DigimonApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, app->rom_name);
    canvas_draw_line(canvas, 0, 12, 127, 12);
    canvas_set_font(canvas, FontSecondary);

    const int visible = 4;
    int first = app->menu_index - visible + 1;
    if(first < 0) first = 0;
    char text[32];
    for(int i = 0; i < visible && first + i < MenuCount; i++) {
        int idx = first + i;
        int y = 15 + i * 12;
        if(idx == app->menu_index) {
            canvas_draw_rbox(canvas, 0, y, 122, 12, 2);
            canvas_set_color(canvas, ColorWhite);
        }
        menu_item_text(app, idx, text, sizeof(text));
        canvas_draw_str(canvas, 4, y + 9, text);
        canvas_set_color(canvas, ColorBlack);
    }
    elements_scrollbar_pos(canvas, 127, 14, 50, app->menu_index, MenuCount);
}

static void draw_catchup(Canvas* canvas, DigimonApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 12, AlignCenter, AlignBottom, "Catching up...");
    canvas_set_font(canvas, FontSecondary);
    char buf[40];
    uint32_t away = app->away_s;
    if(away >= DIGIMON_DAY_S)
        snprintf(buf, sizeof(buf), "Away %lu d %lu h", away / DIGIMON_DAY_S, (away / 3600) % 24);
    else if(away >= 3600)
        snprintf(buf, sizeof(buf), "Away %lu h %lu min", away / 3600, (away / 60) % 60);
    else
        snprintf(buf, sizeof(buf), "Away %lu min %lu s", away / 60, away % 60);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignBottom, buf);
    /* the replay usually ends on the time limit, so show whichever is further along */
    uint32_t pct = app->catchup_total ? (uint64_t)app->catchup_done * 100 / app->catchup_total :
                                        100;
    uint32_t pct_time = (now_ms() - app->catchup_start) * 100u / CATCHUP_MAX_MS;
    if(pct_time > pct) pct = pct_time;
    if(pct > 100) pct = 100;
    elements_progress_bar(canvas, 8, 34, 112, (float)pct / 100.0f);
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, "Back = skip");
}

static void draw_message(Canvas* canvas, DigimonApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 11, AlignCenter, AlignBottom, app->msg_title);
    canvas_draw_line(canvas, 0, 13, 127, 13);
    canvas_set_font(canvas, FontSecondary);
    elements_multiline_text_aligned(canvas, 64, 17, AlignCenter, AlignTop, app->msg_text);
}

static void draw_confirm_reset(Canvas* canvas, DigimonApp* app) {
    UNUSED(app);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 11, AlignCenter, AlignBottom, "Reset Digimon?");
    canvas_draw_line(canvas, 0, 13, 127, 13);
    canvas_set_font(canvas, FontSecondary);
    elements_multiline_text_aligned(
        canvas,
        64,
        18,
        AlignCenter,
        AlignTop,
        "Presses the RESET pin.\nYour Digimon is lost and\na new egg appears.");
    elements_button_left(canvas, "Cancel");
    elements_button_right(canvas, "Reset");
}

static void draw_callback(Canvas* canvas, void* ctx) {
    DigimonApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    switch(app->screen) {
    case ScreenRomList:
        draw_rom_list(canvas, app);
        break;
    case ScreenEmu:
        draw_emu(canvas, app);
        break;
    case ScreenMenu:
        draw_menu(canvas, app);
        break;
    case ScreenCatchup:
        draw_catchup(canvas, app);
        break;
    case ScreenMessage:
        draw_message(canvas, app);
        break;
    case ScreenConfirmReset:
        draw_confirm_reset(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    DigimonApp* app = ctx;
    furi_message_queue_put(app->input_queue, event, 0);
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static void input_rom_list(DigimonApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        if(app->rom_count) app->rom_index = (app->rom_index + app->rom_count - 1) % app->rom_count;
        break;
    case InputKeyDown:
        if(app->rom_count) app->rom_index = (app->rom_index + 1) % app->rom_count;
        break;
    case InputKeyOk:
        if(ev->type == InputTypeShort && app->rom_count) start_game(app, app->rom_index);
        break;
    case InputKeyBack:
        if(ev->type == InputTypeShort) app->running = false;
        break;
    default:
        break;
    }
}

/* Save, then go back to the ROM list (or quit if there is nothing to pick). */
static void save_and_leave(DigimonApp* app) {
    stop_game(app, true);
    scan_roms(app);
    if(app->rom_count > 1)
        app->screen = ScreenRomList;
    else
        app->running = false;
}

static void input_emu(DigimonApp* app, InputEvent* ev) {
    if(ev->key == InputKeyBack) {
        /* tap: the toy's C button. hold: pause menu. The tap is only known
         * once Back is let go, so C is pressed then, not on the press. */
        if(ev->type == InputTypeShort) {
            if(app->taps_queued < TAP_QUEUE_MAX) app->taps_queued++;
            tap_service(app);
        } else if(ev->type == InputTypeLong) {
            release_all_keys(app);
            app->menu_index = MenuResume;
            app->screen = ScreenMenu;
        }
        return;
    }
    if(ev->key >= InputKeyMAX) return;
    if(ev->type == InputTypePress)
        app->keys_down |= (uint8_t)(1 << ev->key);
    else if(ev->type == InputTypeRelease)
        app->keys_down &= (uint8_t) ~(1 << ev->key);
    else
        return;
    update_buttons_from_keys(app);
}

static void menu_change(DigimonApp* app, int dir) {
    switch(app->menu_index) {
    case MenuSound:
        app->settings.sound = (uint8_t)((app->settings.sound + SoundCount + dir) % SoundCount);
        if(!sound_allowed(app)) sound_release(app);
        app->tone_on = app->tone_on && app->speaker;
        settings_save(app);
        break;
    case MenuSpeed: {
        int i = 0;
        while(i < (int)COUNT_OF(speeds) && speeds[i] != app->speed)
            i++;
        i = (i + (int)COUNT_OF(speeds) + dir) % (int)COUNT_OF(speeds);
        app->speed = speeds[i];
        break;
    }
    case MenuLcd:
        app->settings.lcd_style = app->settings.lcd_style ? DigimonRenderSolid : DigimonRenderGrid;
        refresh_frame(app, true);
        settings_save(app);
        break;
    case MenuCatchup:
        app->settings.catch_up = !app->settings.catch_up;
        settings_save(app);
        break;
    default:
        break;
    }
}

static void input_menu(DigimonApp* app, InputEvent* ev) {
    if(ev->type != InputTypeShort && ev->type != InputTypeRepeat) return;
    switch(ev->key) {
    case InputKeyUp:
        app->menu_index = (app->menu_index + MenuCount - 1) % MenuCount;
        break;
    case InputKeyDown:
        app->menu_index = (app->menu_index + 1) % MenuCount;
        break;
    case InputKeyLeft:
        menu_change(app, -1);
        break;
    case InputKeyRight:
        menu_change(app, 1);
        break;
    case InputKeyBack:
        if(ev->type == InputTypeShort) app->screen = ScreenEmu;
        break;
    case InputKeyOk:
        if(ev->type != InputTypeShort) break;
        switch(app->menu_index) {
        case MenuResume:
            app->screen = ScreenEmu;
            break;
        case MenuControls:
            show_message(
                app,
                "Controls",
                "Up/Left: A (select)\nOK: B (confirm)\nDown/Right/Back: C (cancel)\nHold Back: this menu",
                ScreenMenu);
            break;
        case MenuReset:
            app->screen = ScreenConfirmReset;
            break;
        case MenuSaveExit:
            save_and_leave(app);
            break;
        default:
            menu_change(app, 1);
            break;
        }
        break;
    default:
        break;
    }
}

static void handle_input(DigimonApp* app, InputEvent* ev) {
    switch(app->screen) {
    case ScreenRomList:
        input_rom_list(app, ev);
        break;
    case ScreenEmu:
        input_emu(app, ev);
        break;
    case ScreenMenu:
        input_menu(app, ev);
        break;
    case ScreenCatchup:
        if(ev->key == InputKeyBack && ev->type == InputTypeShort) catchup_finish(app);
        break;
    case ScreenMessage:
        if(ev->type == InputTypeShort && (ev->key == InputKeyOk || ev->key == InputKeyBack))
            app->screen = app->msg_return;
        break;
    case ScreenConfirmReset:
        if(ev->type != InputTypeShort) break;
        if(ev->key == InputKeyRight) {
            e0c6200_reset(app->cpu);
            refresh_frame(app, true);
            save_game(app);
            app->screen = ScreenEmu;
        } else if(ev->key == InputKeyLeft || ev->key == InputKeyBack) {
            app->screen = ScreenMenu;
        }
        break;
    }
    view_port_update(app->view_port);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t digimon_app(void* p) {
    UNUSED(p);
    DigimonApp* app = malloc(sizeof(DigimonApp));
    memset(app, 0, sizeof(DigimonApp));

    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, DATA_DIR);
    settings_load(app);
    scan_roms(app);

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(16, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->screen = ScreenRomList;
    app->running = true;
    /* jump straight into the game if there is only one ROM */
    if(app->rom_count == 1) start_game(app, 0);
    view_port_update(app->view_port);

    while(app->running) {
        InputEvent ev;
        bool busy = app->cpu != NULL;
        uint32_t timeout = busy ? furi_ms_to_ticks(LOOP_PERIOD_MS) : FuriWaitForever;
        if(app->screen == ScreenCatchup) timeout = 0;
        /* Handle every queued event before emulating again. While a button
         * is held the ROM polls it nonstop (~180k instructions/s), which can
         * be slower than real time on the Flipper. Taking one event per pass
         * let the key Repeat events fill the queue until the Release was
         * dropped, leaving the button held forever. Stop once the app is
         * exiting: the CPU is already freed then. */
        while(app->running &&
              furi_message_queue_get(app->input_queue, &ev, timeout) == FuriStatusOk) {
            handle_input(app, &ev);
            timeout = 0;
        }
        if(!app->running || !app->cpu) continue;
        if(app->screen == ScreenCatchup)
            catchup_tick(app);
        else
            emu_tick(app);
    }

    stop_game(app, true);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_STORAGE);
    free(app);
    return 0;
}
