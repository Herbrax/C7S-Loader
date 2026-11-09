#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c7s.h"
#include "switch_hid.h"

enum {
    C7S_STEP_WAIT = 0,
    C7S_STEP_KEYBOARD = 1,
    C7S_STEP_GAMEPAD = 2,
    C7S_STEP_STATE = 3,
};

typedef struct {
    uint8_t op;
    uint16_t duration_ms;
    uint8_t modifier;
    uint8_t keycode;
    C7sPadReport pad;
} C7sStep;

typedef struct {
    C7sStep *steps;
    size_t count;
} C7sMacro;

typedef bool (*C7sCancelFn)(void *user);
typedef void (*C7sProgressFn)(unsigned step, unsigned total, void *user);

bool c7s_macro_build(const C7sPayload *payload, C7sMacro *out, char *err, size_t errlen);
void c7s_macro_free(C7sMacro *macro);
bool c7s_macro_run(const C7sMacro *macro, C7sCancelFn cancel, void *cancel_user,
                   C7sProgressFn progress, void *progress_user, char *err, size_t errlen);
