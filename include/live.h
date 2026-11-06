#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "switch_hid.h"

void live_init(void);
bool live_enabled(void);
void live_set_enabled(bool on);
void live_release_all(void);
void live_set_key(uint8_t modifier, uint8_t key);
void live_set_pad(const C7sPadReport *report);
void live_tick(void);
