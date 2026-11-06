#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool initialized, ready, transfer_pending, descriptor_valid, leds_known;
    UsbState state;
    Result error;
    char stage[80];
    uint32_t setup_count, report_requests, reports_completed, reports_failed;
    uint32_t control_in_completed, control_out_completed, bus_resets;
    uint8_t leds, protocol, idle;
    uint8_t last_request_type, last_request;
    uint16_t last_value;
} KeyboardStatus;

typedef struct {
    bool failed;
    int error_number;
    const char *operation;
} KeyboardLogError;

Result keyboard_init(void);
void keyboard_poll(uint64_t now_ms);
void keyboard_exit(void);
const KeyboardStatus *keyboard_status(void);
const KeyboardLogError *keyboard_log_error(void);
bool keyboard_create_log_snapshot(uint64_t now_ms, KeyboardLogError *result);
Result keyboard_send(uint8_t modifier, uint8_t key, uint64_t now_ms);
bool keyboard_idle_due(uint64_t now_ms);
void keyboard_cancel_transfer(void);
void keyboard_log(const char *format, ...);
