#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C7S_KEY_ENTER 0x28
#define C7S_KEY_ESCAPE 0x29
#define C7S_KEY_TAB 0x2B
#define C7S_KEY_SPACE 0x2C
#define C7S_MOD_LSHIFT 0x02

static inline bool c7s_ascii_to_hid(unsigned char c, uint8_t *modifier, uint8_t *keycode) {
    *modifier = 0;
    if (c >= 'a' && c <= 'z') {
        *keycode = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        *keycode = (uint8_t)(0x04 + (c - 'A'));
        *modifier = C7S_MOD_LSHIFT;
        return true;
    }
    if (c >= '1' && c <= '9') {
        *keycode = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    if (c == '0') {
        *keycode = 0x27;
        return true;
    }
    if (c == ' ') {
        *keycode = C7S_KEY_SPACE;
        return true;
    }
    if (c == '\t') {
        *keycode = C7S_KEY_TAB;
        return true;
    }
    const char *shift_digits = "!@#$%^&*()";
    for (int i = 0; i < 10; i++) {
        if (c == (unsigned char)shift_digits[i]) {
            *keycode = (uint8_t)(0x1E + i);
            *modifier = C7S_MOD_LSHIFT;
            return true;
        }
    }
    static const struct {
        char c;
        uint8_t key;
        bool shift;
    } map[] = {
        {'-', 0x2D, false}, {'_', 0x2D, true},
        {'=', 0x2E, false}, {'+', 0x2E, true},
        {'[', 0x2F, false}, {'{', 0x2F, true},
        {']', 0x30, false}, {'}', 0x30, true},
        {'\\', 0x31, false}, {'|', 0x31, true},
        {';', 0x33, false}, {':', 0x33, true},
        {'\'', 0x34, false}, {'"', 0x34, true},
        {'`', 0x35, false}, {'~', 0x35, true},
        {',', 0x36, false}, {'<', 0x36, true},
        {'.', 0x37, false}, {'>', 0x37, true},
        {'/', 0x38, false}, {'?', 0x38, true},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (c == (unsigned char)map[i].c) {
            *keycode = map[i].key;
            *modifier = map[i].shift ? C7S_MOD_LSHIFT : 0;
            return true;
        }
    }
    return false;
}
