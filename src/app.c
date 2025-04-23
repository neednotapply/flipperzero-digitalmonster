#include "furi.h"
#include "gui/gui.h"
#include "gui/elements.h"
#include "digimon_state.h"
#include "notification/notification_messages.h"

typedef enum {
    ROM_VER1,
    ROM_VER2,
    ROM_VER3,
    ROM_COUNT
} RomVersion;

const char* rom_names[ROM_COUNT] = {
    "Digimon Ver. 1",
    "Digimon Ver. 2",
    "Digimon Ver. 3"
};

static RomVersion selected_rom = ROM_VER1;

static void rom_selector_callback(GuiButtonType result, InputType type, void* ctx) {
    if(result == GuiButtonTypeLeft) {
        selected_rom = (selected_rom + ROM_COUNT - 1) % ROM_COUNT;
    } else if(result == GuiButtonTypeRight) {
        selected_rom = (selected_rom + 1) % ROM_COUNT;
    } else if(result == GuiButtonTypeCenter) {
        *((bool*)ctx) = true;
    }
}

int32_t digivice_app(void* p) {
    UNUSED(p);
    bool rom_selected = false;

    Gui* gui = furi_record_open("gui");
    ViewPort* viewport = view_port_alloc();
    view_port_draw_callback_set(viewport, [](Canvas* canvas, void* ctx) {
        UNUSED(ctx);
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 15, AlignCenter, AlignTop, "Select ROM:");
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignTop, rom_names[selected_rom]);
        canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignTop, "< OK >");
    }, NULL);
    view_port_input_callback_set(viewport, rom_selector_callback, &rom_selected);
    gui_add_view_port(gui, viewport);

    while(!rom_selected) {
        furi_delay_ms(100);
    }

    gui_remove_view_port(gui, viewport);
    view_port_free(viewport);

    // Simulated ROM loading
    const char* rom_file = selected_rom == ROM_VER1 ? "DigimonV1.brick" :
                           selected_rom == ROM_VER2 ? "DigimonV2.brick" :
                                                       "DigimonV3.brick";
    furi_log_i("Digivice", "Selected ROM: %s", rom_file);

    // Display loading + hunger info as before
    ViewPort* viewport2 = view_port_alloc();
    view_port_draw_callback_set(viewport2, [](Canvas* canvas, void* ctx) {
        UNUSED(ctx);
        canvas_clear(canvas);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 5, 15, "ROM Loaded:");
        canvas_draw_str(canvas, 5, 30, rom_names[selected_rom]);
        canvas_draw_str(canvas, 5, 45, "Hunger:");
        char hunger_str[16];
        snprintf(hunger_str, sizeof(hunger_str), "%d", digimon_state_get_hunger());
        canvas_draw_str(canvas, 70, 45, hunger_str);
    }, NULL);
    gui_add_view_port(gui, viewport2);
    furi_delay_ms(5000);
    gui_remove_view_port(gui, viewport2);
    view_port_free(viewport2);

    furi_record_close("gui");

    digimon_state_start_daemon();
    return 0;
}
