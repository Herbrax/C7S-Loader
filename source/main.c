#include <switch.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "carry.h"
#include "httpd.h"
#include "ldn_route.h"
#include "live.h"
#include "preload.h"
#include "record.h"
#include "settings.h"
#include "temp_ftp.h"
#include "switch_init.h"
#include "update.h"
#include "ui.h"
#include "usb_composite.h"
#include "usb_worker.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static bool file_exists(const char *name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path)) return false;
    return access(path, R_OK) == 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    mkdir("sdmc:/switch", 0777);
    mkdir(C7S_DIR, 0777);
    mkdir("sdmc:/switch/plume-usb", 0777);
    preload_payloads();
    update_init(envHasArgv() ? (const char *)envGetArgv() : NULL);
    ldn_route_init();

    char ip[64];
    local_ip(ip, sizeof(ip));
    unsigned short port = settings_load_port();
    app_init(port);
    app_set_ip(ip);
    live_init();
    carry_init();
    record_init();
    if (file_exists(C7S_DEFAULT_FILE)) app_set_selected(C7S_DEFAULT_FILE);

    Result rc = usb_composite_init();
    bool worker = false;
    if (R_SUCCEEDED(rc)) {
        worker = usb_worker_start();
        if (!worker) usb_composite_exit();
    }

    if (httpd_start(port) != 0) {
        app_mark_finished(false, "web server failed to start");
    }
    /* TEMP-FTP: diagnostic-only. Remove this call before release. */
    temp_ftp_start();

    app_exit_trace("=== start ===");
    c7s_ui_run();
    app_exit_trace("main: ui returned");
    ldn_route_shutdown();

    httpd_stop();
    app_exit_trace("main: httpd stopped");
    if (worker) usb_worker_stop();
    else usb_composite_exit();
    app_exit_trace("main: worker stopped");
    return 0;
}
