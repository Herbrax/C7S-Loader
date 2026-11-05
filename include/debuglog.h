#pragma once

#include <stddef.h>

void dbg_log_usb(const char *fmt, ...);
void dbg_log_run(const char *fmt, ...);
size_t dbg_snapshot_usb(char *out, size_t cap);
size_t dbg_snapshot_run(char *out, size_t cap);
