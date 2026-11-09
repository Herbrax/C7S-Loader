#include "carry.h"

#include "record.h"
#include "usb_composite.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled;
static bool g_dirty;
static C7sPadReport g_pad;

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

static uint8_t axis8(int v) {
    if (v <= -32768) return 0;
    if (v >= 32767) return 255;
    return (uint8_t)((v + 32768) * 255 / 65535);
}

static uint8_t dpad_hat(uint64_t b) {
    bool up = (b & HidNpadButton_Up) != 0;
    bool down = (b & HidNpadButton_Down) != 0;
    bool left = (b & HidNpadButton_Left) != 0;
    bool right = (b & HidNpadButton_Right) != 0;
    if (up && right) return SWITCH_HAT_UPLEFT;
    if (right && down) return SWITCH_HAT_DOWNLEFT;
    if (down && left) return SWITCH_HAT_DOWNRIGHT;
    if (left && up) return SWITCH_HAT_UPRIGHT;
    if (up) return SWITCH_HAT_UP;
    if (right) return SWITCH_HAT_LEFT;
    if (down) return SWITCH_HAT_DOWN;
    if (left) return SWITCH_HAT_RIGHT;
    return SWITCH_HAT_NOTHING;
}

static void map_pad(PadState *pad, C7sPadReport *out) {
    neutral_pad(out);

    uint64_t b = padGetButtons(pad);
    if (b & HidNpadButton_A) out->buttons |= SWITCH_BTN_A;
    if (b & HidNpadButton_B) out->buttons |= SWITCH_BTN_B;
    if (b & HidNpadButton_X) out->buttons |= SWITCH_BTN_X;
    if (b & HidNpadButton_Y) out->buttons |= SWITCH_BTN_Y;
    if (b & HidNpadButton_L) out->buttons |= SWITCH_BTN_L;
    if (b & HidNpadButton_R) out->buttons |= SWITCH_BTN_R;
    if (b & HidNpadButton_ZL) out->buttons |= SWITCH_BTN_ZL;
    if (b & HidNpadButton_ZR) out->buttons |= SWITCH_BTN_ZR;
    if (b & HidNpadButton_Plus) out->buttons |= SWITCH_BTN_PLUS;
    if (b & HidNpadButton_Minus) out->buttons |= SWITCH_BTN_MINUS;
    if (b & HidNpadButton_StickL) out->buttons |= SWITCH_BTN_L3;
    if (b & HidNpadButton_StickR) out->buttons |= SWITCH_BTN_R3;
    out->hat = dpad_hat(b);

    HidAnalogStickState l = padGetStickPos(pad, 0);
    out->lx = axis8(l.x);
    out->ly = axis8(-l.y);
    HidAnalogStickState r = padGetStickPos(pad, 1);
    out->rx = axis8(r.x);
    out->ry = axis8(-r.y);
}

void carry_init(void) {
    pthread_mutex_lock(&g_lock);
    g_enabled = false;
    g_dirty = false;
    neutral_pad(&g_pad);
    pthread_mutex_unlock(&g_lock);
}

bool carry_enabled(void) {
    pthread_mutex_lock(&g_lock);
    bool on = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return on;
}

void carry_set_enabled(bool on) {
    pthread_mutex_lock(&g_lock);
    if (g_enabled != on) {
        g_enabled = on;
        if (!on) {
            neutral_pad(&g_pad);
            g_dirty = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void carry_update_pad(PadState *pad) {
    if (!pad) return;
    pthread_mutex_lock(&g_lock);
    if (!g_enabled) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    C7sPadReport next;
    map_pad(pad, &next);
    if (memcmp(&g_pad, &next, sizeof(g_pad)) != 0) {
        g_pad = next;
        g_dirty = true;
        pthread_mutex_unlock(&g_lock);
        record_pad(&next);
        return;
    }
    pthread_mutex_unlock(&g_lock);
}

void carry_tick(void) {
    C7sPadReport pad;
    bool enabled = false;
    bool dirty = false;

    pthread_mutex_lock(&g_lock);
    enabled = g_enabled;
    dirty = g_dirty;
    pad = g_pad;
    g_dirty = false;
    pthread_mutex_unlock(&g_lock);

    if (!dirty) return;
    if (!enabled) neutral_pad(&pad);
    usb_send_gamepad(&pad, mono_ms());
}
