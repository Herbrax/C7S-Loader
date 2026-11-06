#pragma once

#include <stdbool.h>

#include "switch_hid.h"

void record_init(void);
bool record_active(void);
bool record_toggle(void);
void record_key(uint8_t modifier, uint8_t key);
void record_pad(const C7sPadReport *pad);
