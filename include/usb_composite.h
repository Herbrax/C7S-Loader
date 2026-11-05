#pragma once

#include <switch.h>
#include <stdbool.h>
#include <stdint.h>

#include "switch_hid.h"

typedef struct {
    bool initialized;
    bool configured;
    bool pad_ready;
    bool kbd_ready;
    bool pad_pending;
    bool kbd_pending;
    UsbState state;
    Result error;
    char stage[96];
    uint32_t pad_sent;
    uint32_t kbd_sent;
    uint32_t reports_failed;
    uint32_t bus_resets;
    uint8_t leds;
    bool leds_known;
} UsbCompositeStatus;

Result usb_composite_init(void);
void usb_composite_poll(uint64_t now_ms);
void usb_composite_exit(void);
const UsbCompositeStatus *usb_composite_status(void);
const char *usb_backend_name(void);

bool usb_keyboard_ready(void);
bool usb_gamepad_ready(void);
bool usb_keyboard_pending(void);
bool usb_gamepad_pending(void);

Result usb_send_keyboard(uint8_t modifier, const uint8_t keycode[6], uint64_t now_ms);
Result usb_send_gamepad(const C7sPadReport *report, uint64_t now_ms);
Result usb_send_keys(uint8_t modifier, uint8_t key1, uint64_t now_ms);
