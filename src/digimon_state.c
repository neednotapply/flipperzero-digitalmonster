#include "digimon_state.h"
#include "furi.h"
#include "notification/notification_app.h"
#include <stdlib.h>

struct DigimonState {
    int hunger;
};

static DigimonState state = { .hunger = 0 };
static bool daemon_running = false;

DigimonState* digimon_state_load() {
    return &state;
}

void digimon_state_save(DigimonState* s) {
    UNUSED(s);
}

int digimon_state_get_hunger() {
    return state.hunger;
}

void daemon_thread(void* p) {
    UNUSED(p);
    NotificationApp* notifications = furi_record_open("notification");
    while(daemon_running) {
        furi_delay_ms(60000);
        state.hunger++;
        notification_message(notifications, &sequence_blink_red_100);
        furi_delay_ms(200);
        notification_message(notifications, &sequence_reset);
    }
    furi_record_close("notification");
}

void digimon_state_start_daemon() {
    if(daemon_running) return;
    daemon_running = true;
    furi_thread_t* thread = furi_thread_alloc();
    furi_thread_set_name(thread, "DigimonDaemon");
    furi_thread_set_stack_size(thread, 1024);
    furi_thread_set_callback(thread, daemon_thread);
    furi_thread_start(thread);
}
