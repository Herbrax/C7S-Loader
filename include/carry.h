#pragma once

#include <stdbool.h>

#include <switch.h>

#include "switch_hid.h"

void carry_init(void);
bool carry_enabled(void);
void carry_set_enabled(bool on);
void carry_update_pad(PadState *pad);
void carry_tick(void);
