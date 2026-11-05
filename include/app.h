#pragma once

#include <stdbool.h>
#include <stddef.h>

#define C7S_DIR "sdmc:/switch/C7S-Loader"
#define C7S_DEFAULT_FILE "RemoteJSLoader.C7S"

typedef struct {
    bool running;
    bool finished;
    bool ok;
    int step;
    int total;
    char selected[128];
    char message[192];
    unsigned short port;
    char ip[64];
} C7sAppSnapshot;

extern volatile bool app_cancel_flag;

void app_init(unsigned short port);
void app_set_ip(const char *ip);

void app_request_start(const char *name);
bool app_take_start(char *name, size_t len);
void app_request_ktest(void);
bool app_take_ktest(void);
void app_request_update(void);
bool app_take_update(void);

void app_request_restart(unsigned short port);
bool app_take_restart(unsigned short *port);

void app_request_cancel(void);

void app_mark_running(const char *name, int total);
void app_set_progress(int step, int total);
void app_mark_finished(bool ok, const char *message);

void app_set_selected(const char *name);
void app_get_selected(char *name, size_t len);

void app_snapshot(C7sAppSnapshot *out);
void app_exit_trace(const char *fmt, ...);
