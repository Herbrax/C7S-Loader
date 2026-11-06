#include "live.h"

#include "record.h"
#include "usb_composite.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled;
static uint8_t g_modifier;
static uint8_t g_key;
static bool g_key_dirty;
static C7sPadReport g_pad;
static bool g_pad_dirty;

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

void live_init(void) {
    pthread_mutex_lock(&g_lock);
    g_enabled = false;
    g_modifier = 0;
    g_key = 0;
    g_key_dirty = false;
    neutral_pad(&g_pad);
    g_pad_dirty = false;
    pthread_mutex_unlock(&g_lock);
}

bool live_enabled(void) {
    pthread_mutex_lock(&g_lock);
    bool on = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return on;
}

void live_set_enabled(bool on) {
    pthread_mutex_lock(&g_lock);
    g_enabled = on;
    if (!on) {
        g_modifier = 0;
        g_key = 0;
        neutral_pad(&g_pad);
        g_key_dirty = true;
        g_pad_dirty = true;
    }
    pthread_mutex_unlock(&g_lock);
}

void live_release_all(void) {
    pthread_mutex_lock(&g_lock);
    g_modifier = 0;
    g_key = 0;
    neutral_pad(&g_pad);
    g_key_dirty = true;
    g_pad_dirty = true;
    pthread_mutex_unlock(&g_lock);
}

void live_set_key(uint8_t modifier, uint8_t key) {
    pthread_mutex_lock(&g_lock);
    if (!g_enabled) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g_modifier == modifier && g_key == key) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_modifier = modifier;
    g_key = key;
    g_key_dirty = true;
    pthread_mutex_unlock(&g_lock);
    record_key(modifier, key);
}

void live_set_pad(const C7sPadReport *report) {
    if (!report) return;
    pthread_mutex_lock(&g_lock);
    if (!g_enabled) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (memcmp(&g_pad, report, sizeof(g_pad)) == 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_pad = *report;
    g_pad_dirty = true;
    pthread_mutex_unlock(&g_lock);
    record_pad(report);
}

void live_tick(void) {
    uint8_t modifier = 0;
    uint8_t key = 0;
    C7sPadReport pad;
    bool key_dirty = false;
    bool pad_dirty = false;

    pthread_mutex_lock(&g_lock);
    if (g_key_dirty) {
        modifier = g_modifier;
        key = g_key;
        g_key_dirty = false;
        key_dirty = true;
    }
    if (g_pad_dirty) {
        pad = g_pad;
        g_pad_dirty = false;
        pad_dirty = true;
    }
    pthread_mutex_unlock(&g_lock);

    if (key_dirty) {
        uint8_t keys[6] = {key, 0, 0, 0, 0, 0};
        usb_send_keyboard(modifier, keys, mono_ms());
    }
    if (pad_dirty) {
        usb_send_gamepad(&pad, mono_ms());
    }
}
