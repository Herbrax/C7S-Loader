#include "macro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "usb_composite.h"

#define KEY_HOLD_MS 40
#define KEY_GAP_MS 40
#define ENTER_HOLD_MS 40
#define ENTER_GAP_MS 200
#define FINAL_RELEASE_MS 50
#define PRE_A_WAIT_MS 800
#define POST_A_WAIT_MS 1200
#define A_PRESS_MS 50
#define PREAMBLE_A_COUNT 5
#define PREAMBLE_GAP_MS 800
#define ESC_TO_A_WAIT_MS 800

#define PLAIN_KEY_HOLD_MS 40
#define PLAIN_KEY_GAP_MS 40
#define PLAIN_ENTER_HOLD_MS 40
#define PLAIN_LINE_DELAY_MS 250

static const C7sPadReport kPadNeutral = {
    .buttons = 0,
    .hat = SWITCH_HAT_NOTHING,
    .lx = SWITCH_STICK_MID,
    .ly = SWITCH_STICK_MID,
    .rx = SWITCH_STICK_MID,
    .ry = SWITCH_STICK_MID,
    .vendor = 0,
};

static const C7sPadReport kPadA = {
    .buttons = SWITCH_BTN_A,
    .hat = SWITCH_HAT_NOTHING,
    .lx = SWITCH_STICK_MID,
    .ly = SWITCH_STICK_MID,
    .rx = SWITCH_STICK_MID,
    .ry = SWITCH_STICK_MID,
    .vendor = 0,
};

static bool push(C7sStep *steps, size_t cap, size_t *count, uint8_t op, uint16_t duration_ms,
                 uint8_t modifier, uint8_t keycode, const C7sPadReport *pad) {
    if (*count >= cap) return false;
    C7sStep *s = &steps[*count];
    memset(s, 0, sizeof(*s));
    s->op = op;
    s->duration_ms = duration_ms;
    s->modifier = modifier;
    s->keycode = keycode;
    if (pad) s->pad = *pad;
    (*count)++;
    return true;
}

bool c7s_macro_build(const C7sPayload *payload, C7sMacro *out, char *err, size_t errlen) {
    memset(out, 0, sizeof(*out));
    size_t total_chars = 0;
    for (int i = 0; i < payload->line_count; i++) total_chars += strlen(payload->lines[i]);
    size_t cap = total_chars * 2 + (size_t)payload->line_count * 16 + 32;
    C7sStep *steps = malloc(cap * sizeof(C7sStep));
    if (!steps) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    size_t n = 0;

    if (payload->script) {
        uint8_t kmod = 0;
        uint8_t kkey = 0;
        C7sPadReport pad;
        memset(&pad, 0, sizeof(pad));
        pad.hat = SWITCH_HAT_NOTHING;
        pad.lx = pad.ly = pad.rx = pad.ry = SWITCH_STICK_MID;
        for (int line = 0; line < payload->line_count; line++) {
            const char *p = payload->lines[line];
            unsigned a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
            if (sscanf(p, "@wait %u", &a) == 1) {
                push(steps, cap, &n, C7S_STEP_STATE, (uint16_t)a, kmod, kkey, &pad);
            } else if (sscanf(p, "@key %u %u", &a, &b) == 2) {
                kmod = (uint8_t)a;
                kkey = (uint8_t)b;
            } else if (sscanf(p, "@pad %u %u %u %u %u %u", &a, &b, &c, &d, &e, &f) == 6) {
                pad.buttons = (uint16_t)a;
                pad.hat = (uint8_t)b;
                pad.lx = (uint8_t)c;
                pad.ly = (uint8_t)d;
                pad.rx = (uint8_t)e;
                pad.ry = (uint8_t)f;
            }
        }
        memset(&pad, 0, sizeof(pad));
        pad.hat = SWITCH_HAT_NOTHING;
        pad.lx = pad.ly = pad.rx = pad.ry = SWITCH_STICK_MID;
        push(steps, cap, &n, C7S_STEP_STATE, FINAL_RELEASE_MS, 0, 0, &pad);
        out->steps = steps;
        out->count = n;
        return true;
    }

    if (payload->plain) {
        for (int line = 0; line < payload->line_count; line++) {
            int prev_key = -1;
            const char *p = payload->lines[line];
            for (; *p; p++) {
                uint8_t modifier = 0;
                uint8_t keycode = 0;
                if (!c7s_ascii_to_hid((unsigned char)*p, &modifier, &keycode)) {
                    snprintf(err, errlen, "unsupported character 0x%02x on line %d",
                             (unsigned char)*p, line + 1);
                    free(steps);
                    return false;
                }
                if (prev_key == (int)keycode) {
                    push(steps, cap, &n, C7S_STEP_WAIT, PLAIN_KEY_GAP_MS, 0, 0, NULL);
                }
                push(steps, cap, &n, C7S_STEP_KEYBOARD, PLAIN_KEY_HOLD_MS, modifier, keycode, NULL);
                prev_key = keycode;
            }
            push(steps, cap, &n, C7S_STEP_KEYBOARD, PLAIN_ENTER_HOLD_MS, 0, C7S_KEY_ENTER, NULL);
            push(steps, cap, &n, C7S_STEP_WAIT, PLAIN_LINE_DELAY_MS, 0, 0, NULL);
        }
        push(steps, cap, &n, C7S_STEP_WAIT, FINAL_RELEASE_MS, 0, 0, NULL);
        out->steps = steps;
        out->count = n;
        return true;
    }

    for (int i = 0; i < PREAMBLE_A_COUNT; i++) {
        push(steps, cap, &n, C7S_STEP_GAMEPAD, A_PRESS_MS, 0, 0, &kPadA);
        push(steps, cap, &n, C7S_STEP_WAIT, PREAMBLE_GAP_MS, 0, 0, NULL);
    }

    for (int line = 0; line < payload->line_count; line++) {
        push(steps, cap, &n, C7S_STEP_KEYBOARD, KEY_HOLD_MS, 0, C7S_KEY_ESCAPE, NULL);
        push(steps, cap, &n, C7S_STEP_WAIT, ESC_TO_A_WAIT_MS, 0, 0, NULL);
        push(steps, cap, &n, C7S_STEP_GAMEPAD, A_PRESS_MS, 0, 0, &kPadA);
        push(steps, cap, &n, C7S_STEP_WAIT, POST_A_WAIT_MS, 0, 0, NULL);

        int prev_key = -1;
        const char *p = payload->lines[line];
        for (; *p; p++) {
            uint8_t modifier = 0;
            uint8_t keycode = 0;
            if (!c7s_ascii_to_hid((unsigned char)*p, &modifier, &keycode)) {
                snprintf(err, errlen, "unsupported character 0x%02x on line %d",
                         (unsigned char)*p, line + 1);
                free(steps);
                return false;
            }
            if (prev_key == (int)keycode) {
                push(steps, cap, &n, C7S_STEP_WAIT, KEY_GAP_MS, 0, 0, NULL);
            }
            push(steps, cap, &n, C7S_STEP_KEYBOARD, KEY_HOLD_MS, modifier, keycode, NULL);
            prev_key = keycode;
        }

        push(steps, cap, &n, C7S_STEP_KEYBOARD, ENTER_HOLD_MS, 0, C7S_KEY_ENTER, NULL);
        push(steps, cap, &n, C7S_STEP_WAIT, ENTER_GAP_MS, 0, 0, NULL);
        push(steps, cap, &n, C7S_STEP_KEYBOARD, ENTER_HOLD_MS, 0, C7S_KEY_ENTER, NULL);

        if (line + 1 < payload->line_count) {
            push(steps, cap, &n, C7S_STEP_WAIT, PRE_A_WAIT_MS, 0, 0, NULL);
            push(steps, cap, &n, C7S_STEP_GAMEPAD, A_PRESS_MS, 0, 0, &kPadA);
            push(steps, cap, &n, C7S_STEP_WAIT, POST_A_WAIT_MS, 0, 0, NULL);
        }
    }

    push(steps, cap, &n, C7S_STEP_WAIT, FINAL_RELEASE_MS, 0, 0, NULL);
    out->steps = steps;
    out->count = n;
    return true;
}

void c7s_macro_free(C7sMacro *macro) {
    if (!macro) return;
    free(macro->steps);
    memset(macro, 0, sizeof(*macro));
}

static uint64_t mono_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static bool cancelled(C7sCancelFn cancel, void *cancel_user) {
    return cancel && cancel(cancel_user);
}

static bool sleep_ms(uint32_t ms, C7sCancelFn cancel, void *cancel_user) {
    uint64_t end = mono_ms() + ms;
    while (mono_ms() < end) {
        usb_composite_poll(mono_ms());
        if (cancelled(cancel, cancel_user)) return false;
        svcSleepThread(1000000ull);
    }
    usb_composite_poll(mono_ms());
    return !cancelled(cancel, cancel_user);
}

static bool send_kbd(uint8_t modifier, uint8_t keycode, C7sCancelFn cancel, void *cancel_user) {
    (void)cancel;
    (void)cancel_user;
    uint8_t keys[6] = {keycode, 0, 0, 0, 0, 0};
    return R_SUCCEEDED(usb_send_keyboard(modifier, keys, mono_ms()));
}

static bool send_pad(const C7sPadReport *pad, C7sCancelFn cancel, void *cancel_user) {
    (void)cancel;
    (void)cancel_user;
    return R_SUCCEEDED(usb_send_gamepad(pad, mono_ms()));
}

bool c7s_macro_run(const C7sMacro *macro, C7sCancelFn cancel, void *cancel_user,
                   C7sProgressFn progress, void *progress_user, char *err, size_t errlen) {
    if (!usb_keyboard_ready() || !usb_gamepad_ready()) {
        snprintf(err, errlen, "USB interfaces are not ready");
        return false;
    }

    C7sKbdReport last_kbd;
    memset(&last_kbd, 0, sizeof(last_kbd));
    C7sPadReport last_pad = kPadNeutral;
    bool ok = true;

    for (size_t i = 0; i < macro->count; i++) {
        if (cancelled(cancel, cancel_user)) {
            snprintf(err, errlen, "cancelled");
            ok = false;
            break;
        }
        const C7sStep *step = &macro->steps[i];
        if (progress) progress((unsigned)i, (unsigned)macro->count, progress_user);

        C7sKbdReport want_kbd;
        memset(&want_kbd, 0, sizeof(want_kbd));
        C7sPadReport want_pad = kPadNeutral;

        if (step->op == C7S_STEP_KEYBOARD) {
            want_kbd.modifier = step->modifier;
            want_kbd.keycode[0] = step->keycode;
        } else if (step->op == C7S_STEP_GAMEPAD) {
            want_pad = step->pad;
        } else if (step->op == C7S_STEP_STATE) {
            want_kbd.modifier = step->modifier;
            want_kbd.keycode[0] = step->keycode;
            want_pad = step->pad;
        }

        bool kbd_changed = memcmp(&want_kbd, &last_kbd, sizeof(want_kbd)) != 0;
        bool pad_changed = memcmp(&want_pad, &last_pad, sizeof(want_pad)) != 0;

        if (kbd_changed && !send_kbd(want_kbd.modifier, want_kbd.keycode[0], cancel, cancel_user)) {
            snprintf(err, errlen, "keyboard transfer failed at step %u", (unsigned)i);
            ok = false;
            break;
        }
        if (pad_changed && !send_pad(&want_pad, cancel, cancel_user)) {
            snprintf(err, errlen, "gamepad transfer failed at step %u", (unsigned)i);
            ok = false;
            break;
        }

        last_kbd = want_kbd;
        last_pad = want_pad;

        if (!sleep_ms(step->duration_ms, cancel, cancel_user)) {
            snprintf(err, errlen, "cancelled");
            ok = false;
            break;
        }
    }

    /* Always release whatever was left held, even on cancel or a mid-run
       failure, so the target console is never left with a stuck key or
       button. Forced through (cancel flag ignored) so a cancel still lifts
       the last key. */
    bool kbd_held = last_kbd.modifier != 0 || last_kbd.keycode[0] != 0;
    bool pad_held = memcmp(&last_pad, &kPadNeutral, sizeof(last_pad)) != 0;
    if (kbd_held) send_kbd(0, 0, NULL, NULL);
    if (pad_held) send_pad(&kPadNeutral, NULL, NULL);
    return ok;
}
