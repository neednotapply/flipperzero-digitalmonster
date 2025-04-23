#pragma once

#include <stdint.h>

typedef struct DigimonState DigimonState;

DigimonState* digimon_state_load();
void digimon_state_save(DigimonState* state);
int digimon_state_get_hunger();
void digimon_state_start_daemon();
