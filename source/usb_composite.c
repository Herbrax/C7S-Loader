#include "usb_composite.h"

#include "debuglog.h"
#include "usb_keyboard.h"
#include "usb_gamepad.h"
#include "switch_gamepad.h"

#include <stdio.h>
#include <string.h>

static UsbCompositeStatus g_status;

static uint64_t mono_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static void sync_status(void) {
    const KeyboardStatus *k = keyboard_status();
    const GamepadStatus *g = gamepad_status();

    g_status.initialized = k->initialized;
    g_status.configured = k->ready && gamepad_ready();
    g_status.pad_ready = gamepad_ready();
    g_status.kbd_ready = k->ready;
    g_status.pad_pending = g->transfer_pending;
    g_status.kbd_pending = k->transfer_pending;
    g_status.state = k->state;
    g_status.error = k->error;
    snprintf(g_status.stage, sizeof(g_status.stage), "%s", k->stage);
    g_status.kbd_sent = k->reports_completed;
    g_status.reports_failed = k->reports_failed;
    g_status.bus_resets = k->bus_resets;
    g_status.leds = k->leds;
    g_status.leds_known = k->leds_known;
}

Result usb_composite_init(void) {
    memset(&g_status, 0, sizeof(g_status));
    dbg_log_usb("backend: RecurCiv composite (HORI gamepad + Plume keyboard)");

    Result rc = keyboard_init();
    if (R_FAILED(rc)) {
        g_status.error = rc;
        snprintf(g_status.stage, sizeof(g_status.stage), "keyboard_init failed");
        dbg_log_usb("keyboard_init failed: %08x", (unsigned)rc);
        return rc;
    }

    rc = gamepad_init();
    if (R_FAILED(rc)) {
        g_status.error = rc;
        snprintf(g_status.stage, sizeof(g_status.stage), "gamepad_init failed");
        dbg_log_usb("gamepad_init failed: %08x", (unsigned)rc);
        keyboard_exit();
        return rc;
    }

    g_status.initialized = true;
    sync_status();
    dbg_log_usb("RecurCiv composite initialized");
    return 0;
}

void usb_composite_poll(uint64_t now_ms) {
    keyboard_poll(now_ms);
    gamepad_poll(now_ms);
    sync_status();
}

void usb_composite_exit(void) {
    gamepad_exit();
    keyboard_exit();
    g_status.initialized = false;
    g_status.configured = false;
    g_status.pad_ready = false;
    g_status.kbd_ready = false;
    g_status.pad_pending = false;
    g_status.kbd_pending = false;
    dbg_log_usb("RecurCiv composite exited");
}

const UsbCompositeStatus *usb_composite_status(void) {
    sync_status();
    return &g_status;
}

const char *usb_backend_name(void) {
    return "RecurCiv composite";
}

bool usb_keyboard_ready(void) {
    const KeyboardStatus *k = keyboard_status();
    return k->initialized && k->ready;
}

bool usb_gamepad_ready(void) {
    return gamepad_ready();
}

bool usb_keyboard_pending(void) {
    return keyboard_status()->transfer_pending;
}

bool usb_gamepad_pending(void) {
    return gamepad_status()->transfer_pending;
}

static bool wait_keyboard_idle(uint32_t timeout_ms) {
    uint64_t start = mono_ms();
    while (keyboard_status()->transfer_pending) {
        if (mono_ms() - start > timeout_ms) return false;
        keyboard_poll(mono_ms());
        svcSleepThread(500000ull);
    }
    return true;
}

Result usb_send_keyboard(uint8_t modifier, const uint8_t keycode[6], uint64_t now_ms) {
    (void)now_ms;
    if (!usb_keyboard_ready())
        return KERNELRESULT(InvalidState);

    uint8_t key = 0;
    for (int i = 0; i < 6; i++) {
        if (keycode[i]) {
            key = keycode[i];
            break;
        }
    }

    if (!wait_keyboard_idle(250))
        return MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite);

    Result rc = keyboard_send(modifier, key, mono_ms());
    if (R_FAILED(rc)) return rc;

    if (!wait_keyboard_idle(250))
        return MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite);
    return 0;
}

Result usb_send_gamepad(const C7sPadReport *report, uint64_t now_ms) {
    (void)now_ms;
    if (!report) return -1;
    if (!gamepad_ready()) return KERNELRESULT(InvalidState);

    SwitchGamepadReport r;
    r.buttons = report->buttons;
    r.hat = report->hat;
    r.lx = report->lx;
    r.ly = report->ly;
    r.rx = report->rx;
    r.ry = report->ry;
    r.vendor = report->vendor;
    gamepad_set_state(&r);
    g_status.pad_sent++;
    return 0;
}

Result usb_send_keys(uint8_t modifier, uint8_t key1, uint64_t now_ms) {
    uint8_t keys[6] = {key1, 0, 0, 0, 0, 0};
    return usb_send_keyboard(modifier, keys, now_ms);
}
