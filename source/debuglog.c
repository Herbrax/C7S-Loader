#include "debuglog.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DBG_CAP 32768

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_usb[DBG_CAP + 1];
static size_t g_usb_len;
static char g_run[DBG_CAP + 1];
static size_t g_run_len;

static void append_line(char *buf, size_t *len, const char *fmt, va_list args) {
    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    line[n++] = '\n';
    if ((size_t)n >= DBG_CAP) {
        memcpy(buf, line + (n - DBG_CAP), DBG_CAP);
        *len = DBG_CAP;
        buf[*len] = '\0';
        return;
    }
    if (*len + (size_t)n > DBG_CAP) {
        size_t drop = *len + (size_t)n - DBG_CAP;
        memmove(buf, buf + drop, *len - drop);
        *len -= drop;
    }
    memcpy(buf + *len, line, (size_t)n);
    *len += (size_t)n;
    buf[*len] = '\0';
}

void dbg_log_usb(const char *fmt, ...) {
    pthread_mutex_lock(&g_lock);
    va_list args;
    va_start(args, fmt);
    append_line(g_usb, &g_usb_len, fmt, args);
    va_end(args);
    pthread_mutex_unlock(&g_lock);
}

void dbg_log_run(const char *fmt, ...) {
    pthread_mutex_lock(&g_lock);
    va_list args;
    va_start(args, fmt);
    append_line(g_run, &g_run_len, fmt, args);
    va_end(args);
    pthread_mutex_unlock(&g_lock);
}

size_t dbg_snapshot_usb(char *out, size_t cap) {
    size_t n;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (cap == 1) return 0;
    pthread_mutex_lock(&g_lock);
    n = g_usb_len < cap - 1 ? g_usb_len : cap - 1;
    memcpy(out, g_usb + (g_usb_len - n), n);
    out[n] = '\0';
    pthread_mutex_unlock(&g_lock);
    return n;
}

size_t dbg_snapshot_run(char *out, size_t cap) {
    size_t n;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (cap == 1) return 0;
    pthread_mutex_lock(&g_lock);
    n = g_run_len < cap - 1 ? g_run_len : cap - 1;
    memcpy(out, g_run + (g_run_len - n), n);
    out[n] = '\0';
    pthread_mutex_unlock(&g_lock);
    return n;
}
