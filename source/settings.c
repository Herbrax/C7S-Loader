#include "settings.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned short settings_load_port(void) {
    FILE *f = fopen(C7S_SETTINGS_PATH, "rb");
    if (!f) return C7S_DEFAULT_PORT;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *key = strstr(buf, "\"port\"");
    if (!key) return C7S_DEFAULT_PORT;
    const char *colon = strchr(key, ':');
    if (!colon) return C7S_DEFAULT_PORT;
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return C7S_DEFAULT_PORT;
    return (unsigned short)port;
}

bool settings_save_port(unsigned short port) {
    if (port == 0) return false;
    FILE *f = fopen(C7S_SETTINGS_PATH, "wb");
    if (!f) return false;
    int written = fprintf(f, "{\"port\":%u}\n", (unsigned)port);
    fclose(f);
    return written > 0;
}
