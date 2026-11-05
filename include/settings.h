#pragma once

#include <stdbool.h>

#define C7S_DEFAULT_PORT 6969
#define C7S_SETTINGS_PATH "sdmc:/switch/C7S-Loader/settings.json"

unsigned short settings_load_port(void);
bool settings_save_port(unsigned short port);
