#include "preload.h"

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "preload_data.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static void write_if_missing(const char *name, const unsigned char *data, size_t len) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path)) return;
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
}

void preload_payloads(void) {
    mkdir(C7S_DIR, 0777);
    write_if_missing("CIV7-RemoteJSLoader.C7S", c7s_preload_remote_js, c7s_preload_remote_js_len);
    write_if_missing("CIV7-Reach-Chat-Input.C7S", c7s_preload_reach_chat, c7s_preload_reach_chat_len);
}
