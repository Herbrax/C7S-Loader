#include "record.h"

#include <switch.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active;
static uint64_t g_start_ms;
static uint64_t g_last_ms;
static uint8_t g_modifier;
static uint8_t g_key;
static C7sPadReport g_pad;
static char **g_lines;
static size_t g_count;
static size_t g_cap;

static uint64_t mono_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static void neutral_pad(C7sPadReport *pad) {
    memset(pad, 0, sizeof(*pad));
    pad->hat = SWITCH_HAT_NOTHING;
    pad->lx = SWITCH_STICK_MID;
    pad->ly = SWITCH_STICK_MID;
    pad->rx = SWITCH_STICK_MID;
    pad->ry = SWITCH_STICK_MID;
}

static void free_lines(void) {
    for (size_t i = 0; i < g_count; i++) free(g_lines[i]);
    free(g_lines);
    g_lines = NULL;
    g_count = 0;
    g_cap = 0;
}

static bool add_line(const char *line) {
    if (g_count == g_cap) {
        size_t next = g_cap ? g_cap * 2 : 64;
        char **p = realloc(g_lines, next * sizeof(char *));
        if (!p) return false;
        g_lines = p;
        g_cap = next;
    }
    g_lines[g_count] = strdup(line);
    if (!g_lines[g_count]) return false;
    g_count++;
    return true;
}

static void add_wait(void) {
    uint64_t now = mono_ms();
    uint64_t elapsed = now - g_last_ms;
    if (elapsed > 65535) elapsed = 65535;
    char line[32];
    snprintf(line, sizeof(line), "@wait %llu", (unsigned long long)elapsed);
    add_line(line);
    g_last_ms = now;
}

static void add_key(void) {
    char line[32];
    snprintf(line, sizeof(line), "@key %u %u", (unsigned)g_modifier, (unsigned)g_key);
    add_line(line);
}

static void add_pad(void) {
    char line[96];
    snprintf(line, sizeof(line), "@pad %u %u %u %u %u %u",
             (unsigned)g_pad.buttons, (unsigned)g_pad.hat,
             (unsigned)g_pad.lx, (unsigned)g_pad.ly,
             (unsigned)g_pad.rx, (unsigned)g_pad.ry);
    add_line(line);
}

static void stop_locked(void) {
    g_active = false;
    char path[256];
    uint64_t stamp = g_start_ms;
    snprintf(path, sizeof(path), "sdmc:/switch/C7S-Loader/Recorded-%llu.C7S",
             (unsigned long long)stamp);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "Recorded - %u\n---BEGIN---\n", (unsigned)g_count);
        for (size_t i = 0; i < g_count; i++) fprintf(f, "%s\n", g_lines[i]);
        fprintf(f, "---END---\n");
        fclose(f);
    }
    free_lines();
}

bool record_toggle(void) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        stop_locked();
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g_active = true;
    g_start_ms = mono_ms();
    g_last_ms = g_start_ms;
    g_modifier = 0;
    g_key = 0;
    neutral_pad(&g_pad);
    free_lines();
    add_line("@wait 0");
    add_key();
    add_pad();
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool record_active(void) {
    pthread_mutex_lock(&g_lock);
    bool active = g_active;
    pthread_mutex_unlock(&g_lock);
    return active;
}

void record_key(uint8_t modifier, uint8_t key) {
    pthread_mutex_lock(&g_lock);
    if (!g_active) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g_modifier == modifier && g_key == key) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    add_wait();
    g_modifier = modifier;
    g_key = key;
    add_key();
    pthread_mutex_unlock(&g_lock);
}

void record_pad(const C7sPadReport *pad) {
    if (!pad) return;
    pthread_mutex_lock(&g_lock);
    if (!g_active) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (memcmp(&g_pad, pad, sizeof(g_pad)) == 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    add_wait();
    g_pad = *pad;
    add_pad();
    pthread_mutex_unlock(&g_lock);
}

void record_init(void) {
    pthread_mutex_lock(&g_lock);
    g_active = false;
    g_modifier = 0;
    g_key = 0;
    neutral_pad(&g_pad);
    free_lines();
    pthread_mutex_unlock(&g_lock);
}
