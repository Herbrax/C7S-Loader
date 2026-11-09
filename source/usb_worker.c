#include "usb_worker.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "app.h"
#include "carry.h"
#include "c7s.h"
#include "debuglog.h"
#include "keymap.h"
#include "live.h"
#include "macro.h"
#include "usb_composite.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static pthread_t g_thread;
static volatile bool g_stop;
static bool g_started;
static FILE *g_runlog;

static void run_log(const char *fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    dbg_log_run("%s", line);
    if (!g_runlog) return;
    fprintf(g_runlog, "%s\n", line);
    fflush(g_runlog);
}

static unsigned g_progress_last;
static uint64_t g_progress_ms;

static uint64_t now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static bool cancel_cb(void *user) {
    (void)user;
    return app_cancel_flag || g_stop;
}

static void progress_cb(unsigned step, unsigned total, void *user) {
    (void)user;
    app_set_progress((int)step, (int)total);
    uint64_t now = now_ms();
    if (step == 0 || step - g_progress_last >= 200 || now - g_progress_ms >= 1000) {
        run_log("progress %u/%u", step, total);
        g_progress_last = step;
        g_progress_ms = now;
    }
}

static void countdown(unsigned seconds) {
    uint64_t end = now_ms() + (uint64_t)seconds * 1000;
    while (now_ms() < end) {
        usb_composite_poll(now_ms());
        if (app_cancel_flag || g_stop) return;
        svcSleepThread(10000000ull);
    }
}

static void run_keyboard_test(void) {
    app_mark_running("keyboard test", 0);
    run_log("kbdtest start");
    g_progress_last = 0;
    g_progress_ms = now_ms();
    countdown(5);
    if (g_stop || app_cancel_flag) {
        app_mark_finished(false, "cancelled");
        return;
    }
    if (!usb_keyboard_ready()) {
        app_mark_finished(false, "keyboard not ready");
        run_log("kbdtest not ready");
        return;
    }
    C7sStep steps[32];
    size_t n = 0;
    const char *text = "hello";
    for (const char *p = text; *p; p++) {
        uint8_t mod = 0, key = 0;
        if (!c7s_ascii_to_hid((unsigned char)*p, &mod, &key)) continue;
        memset(&steps[n], 0, sizeof(steps[n]));
        steps[n].op = C7S_STEP_KEYBOARD;
        steps[n].duration_ms = 80;
        steps[n].modifier = mod;
        steps[n].keycode = key;
        n++;
        memset(&steps[n], 0, sizeof(steps[n]));
        steps[n].op = C7S_STEP_WAIT;
        steps[n].duration_ms = 80;
        n++;
    }
    C7sMacro macro = {steps, n};
    char err[192] = {0};
    bool ok = c7s_macro_run(&macro, cancel_cb, NULL, progress_cb, NULL, err, sizeof(err));
    app_mark_finished(ok, ok ? "keyboard test sent" : err);
    const UsbCompositeStatus *u = usb_composite_status();
    run_log("kbdtest done ok=%d err=%s pad_sent=%u kbd_sent=%u failed=%u",
            ok ? 1 : 0, err, (unsigned)u->pad_sent, (unsigned)u->kbd_sent,
            (unsigned)u->reports_failed);
}

static void run_payload(const char *name) {
    char path[PATH_MAX];
    if (!name || !name[0] || snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path)) {
        app_mark_finished(false, "invalid payload name");
        return;
    }

    C7sPayload payload;
    if (!c7s_parse_file(path, &payload)) {
        app_mark_finished(false, payload.error);
        return;
    }
    C7sMacro macro;
    char err[192] = {0};
    if (!c7s_macro_build(&payload, &macro, err, sizeof(err))) {
        app_mark_finished(false, err);
        c7s_free(&payload);
        return;
    }

    app_mark_running(name, (int)macro.count);
    run_log("run start %s steps=%u", name, (unsigned)macro.count);
    g_progress_last = 0;
    g_progress_ms = now_ms();
    countdown(5);
    if (g_stop || app_cancel_flag) {
        app_mark_finished(false, "cancelled");
        c7s_macro_free(&macro);
        c7s_free(&payload);
        return;
    }
    bool ok = c7s_macro_run(&macro, cancel_cb, NULL, progress_cb, NULL, err, sizeof(err));
    app_mark_finished(ok, ok ? "payload finished" : err);
    const UsbCompositeStatus *u = usb_composite_status();
    run_log("run done ok=%d err=%s pad_sent=%u kbd_sent=%u failed=%u",
            ok ? 1 : 0, err, (unsigned)u->pad_sent, (unsigned)u->kbd_sent,
            (unsigned)u->reports_failed);

    c7s_macro_free(&macro);
    c7s_free(&payload);
}

static void *worker(void *arg) {
    (void)arg;
    rename("sdmc:/switch/C7S-Loader/run.log", "sdmc:/switch/C7S-Loader/run-prev.log");
    g_runlog = fopen("sdmc:/switch/C7S-Loader/run.log", "w");
    run_log("worker start");
    while (!g_stop) {
        char name[128];
        if (app_take_start(name, sizeof(name))) {
            live_release_all();
            live_tick();
            run_payload(name);
            continue;
        }
        if (app_take_ktest()) {
            live_release_all();
            live_tick();
            run_keyboard_test();
            continue;
        }
        usb_composite_poll(now_ms());
        live_tick();
        carry_tick();
        svcSleepThread(1000000ull);
    }
    run_log("worker stop");
    if (g_runlog) {
        fclose(g_runlog);
        g_runlog = NULL;
    }
    usb_composite_exit();
    return NULL;
}

bool usb_worker_start(void) {
    if (g_started) return true;
    g_stop = false;
    if (pthread_create(&g_thread, NULL, worker, NULL) != 0) return false;
    g_started = true;
    return true;
}

void usb_worker_stop(void) {
    if (!g_started) return;
    g_stop = true;
    app_cancel_flag = true;
    pthread_join(g_thread, NULL);
    g_started = false;
}
