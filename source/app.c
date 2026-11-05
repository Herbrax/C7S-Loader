#include "app.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

volatile bool app_cancel_flag = false;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_start_requested;
static bool g_ktest_requested;
static bool g_update_requested;
static bool g_restart_requested;
static unsigned short g_pending_port;
static bool g_running;
static bool g_finished;
static bool g_ok;
static int g_step;
static int g_total;
static char g_selected[128];
static char g_message[192];
static unsigned short g_port;
static char g_ip[64];

void app_init(unsigned short port) {
    pthread_mutex_lock(&g_lock);
    g_start_requested = false;
    g_ktest_requested = false;
    g_update_requested = false;
    g_restart_requested = false;
    g_pending_port = port;
    g_running = false;
    g_finished = false;
    g_ok = false;
    g_step = 0;
    g_total = 0;
    g_selected[0] = '\0';
    g_message[0] = '\0';
    g_port = port;
    g_ip[0] = '\0';
    app_cancel_flag = false;
    pthread_mutex_unlock(&g_lock);
}

void app_set_ip(const char *ip) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_ip, sizeof(g_ip), "%s", ip ? ip : "");
    pthread_mutex_unlock(&g_lock);
}

void app_request_start(const char *name) {
    pthread_mutex_lock(&g_lock);
    if (!g_running && name && name[0]) {
        snprintf(g_selected, sizeof(g_selected), "%s", name);
        g_start_requested = true;
    }
    pthread_mutex_unlock(&g_lock);
}

bool app_take_start(char *name, size_t len) {
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_start_requested && !g_running) {
        snprintf(name, len, "%s", g_selected);
        g_start_requested = false;
    g_ktest_requested = false;
    g_update_requested = false;
        g_running = true;
        app_cancel_flag = false;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

void app_request_ktest(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_running) g_ktest_requested = true;
    pthread_mutex_unlock(&g_lock);
}

bool app_take_ktest(void) {
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_ktest_requested && !g_running) {
        g_ktest_requested = false;
    g_update_requested = false;
        g_running = true;
        app_cancel_flag = false;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

void app_request_update(void) {
    pthread_mutex_lock(&g_lock);
    g_update_requested = true;
    pthread_mutex_unlock(&g_lock);
}

bool app_take_update(void) {
    bool got;
    pthread_mutex_lock(&g_lock);
    got = g_update_requested;
    g_update_requested = false;
    pthread_mutex_unlock(&g_lock);
    return got;
}

void app_request_restart(unsigned short port) {
    pthread_mutex_lock(&g_lock);
    g_pending_port = port;
    g_restart_requested = true;
    pthread_mutex_unlock(&g_lock);
}

bool app_take_restart(unsigned short *port) {
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_restart_requested) {
        *port = g_pending_port;
        g_restart_requested = false;
        g_port = g_pending_port;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

void app_request_cancel(void) {
    app_cancel_flag = true;
    pthread_mutex_lock(&g_lock);
    snprintf(g_message, sizeof(g_message), "cancel requested");
    pthread_mutex_unlock(&g_lock);
}

void app_mark_running(const char *name, int total) {
    pthread_mutex_lock(&g_lock);
    g_running = true;
    g_finished = false;
    g_ok = false;
    g_step = 0;
    g_total = total;
    g_message[0] = '\0';
    snprintf(g_selected, sizeof(g_selected), "%s", name ? name : "");
    pthread_mutex_unlock(&g_lock);
}

void app_set_progress(int step, int total) {
    pthread_mutex_lock(&g_lock);
    g_step = step;
    g_total = total;
    pthread_mutex_unlock(&g_lock);
}

void app_mark_finished(bool ok, const char *message) {
    pthread_mutex_lock(&g_lock);
    g_running = false;
    g_finished = true;
    g_ok = ok;
    snprintf(g_message, sizeof(g_message), "%s", message ? message : "");
    app_cancel_flag = false;
    pthread_mutex_unlock(&g_lock);
}

void app_set_selected(const char *name) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_selected, sizeof(g_selected), "%s", name ? name : "");
    pthread_mutex_unlock(&g_lock);
}

void app_get_selected(char *name, size_t len) {
    pthread_mutex_lock(&g_lock);
    snprintf(name, len, "%s", g_selected);
    pthread_mutex_unlock(&g_lock);
}

void app_exit_trace(const char *fmt, ...) {
    FILE *f = fopen("sdmc:/switch/C7S-Loader/exit.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

void app_snapshot(C7sAppSnapshot *out) {
    pthread_mutex_lock(&g_lock);
    out->running = g_running;
    out->finished = g_finished;
    out->ok = g_ok;
    out->step = g_step;
    out->total = g_total;
    snprintf(out->selected, sizeof(out->selected), "%s", g_selected);
    snprintf(out->message, sizeof(out->message), "%s", g_message);
    out->port = g_port;
    snprintf(out->ip, sizeof(out->ip), "%s", g_ip);
    pthread_mutex_unlock(&g_lock);
}
