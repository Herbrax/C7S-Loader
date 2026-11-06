#pragma once

#include <switch.h>
#include <stdbool.h>

typedef struct {
    bool configured;
    bool descriptor_valid;
    bool endpoint_available;
    bool transfer_pending;
} GamepadStatus;

const GamepadStatus *gamepad_status(void);
#include "switch_gamepad.h"

Result gamepad_init(void);
void gamepad_poll(uint64_t now_ms);
void gamepad_set_state(const SwitchGamepadReport *report);
Result gamepad_send(const SwitchGamepadReport *report, uint64_t now_ms);
void gamepad_exit(void);
bool gamepad_ready(void);

/* Exclusive session: each accepted state must complete before the next one.
   INTERRUPTED retains ownership until stop is requested after reconnection. */
typedef enum {
    GAMEPAD_EXCLUSIVE_OFF, GAMEPAD_EXCLUSIVE_SYNC, GAMEPAD_EXCLUSIVE_READY,
    GAMEPAD_EXCLUSIVE_PENDING, GAMEPAD_EXCLUSIVE_STOPPING,
    GAMEPAD_EXCLUSIVE_INTERRUPTED
} GamepadExclusiveState;
GamepadExclusiveState gamepad_exclusive_status(void);
bool gamepad_exclusive_begin(void);
bool gamepad_exclusive_submit(const SwitchGamepadReport *report);
bool gamepad_exclusive_stop(void);
