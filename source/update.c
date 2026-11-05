#include "update.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static char g_self[PATH_MAX + 16];
static char g_new[PATH_MAX + 16];

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void parse_self(const char *argv) {
    g_self[0] = '\0';
    if (!argv) return;
    const char *p = argv;
    while (*p == ' ') p++;
    char first[PATH_MAX];
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < sizeof(first)) first[n++] = *p++;
    } else {
        while (*p && *p != ' ' && n + 1 < sizeof(first)) first[n++] = *p++;
    }
    first[n] = '\0';
    if (n < 5) return;
    if (strcasecmp(first + n - 4, ".nro") != 0) return;
    if (first[0] == '/') snprintf(g_self, sizeof(g_self), "sdmc:%s", first);
    else snprintf(g_self, sizeof(g_self), "%s", first);
    if (!path_exists(g_self)) g_self[0] = '\0';
}

void update_init(const char *argv) {
    parse_self(argv);
    if (!g_self[0]) {
        snprintf(g_self, sizeof(g_self), "sdmc:/switch/c7s-loader.nro");
        if (!path_exists(g_self)) g_self[0] = '\0';
    }
    if (!g_self[0]) return;
    snprintf(g_new, sizeof(g_new), "%s", g_self);
    size_t len = strlen(g_new);
    if (len > 4 && strcasecmp(g_new + len - 4, ".nro") == 0) g_new[len - 4] = '\0';
    len = strlen(g_new);
    snprintf(g_new + len, sizeof(g_new) - len, ".new.nro");
    update_cleanup();
}

void update_cleanup(void) {
    if (!g_self[0]) return;
    char old[PATH_MAX + 64];
    snprintf(old, sizeof(old), "%s.old", g_self);
    unlink(old);
    unlink(g_new);
}

bool update_new_path(char *out, size_t cap) {
    if (!g_self[0] || !out || cap == 0) return false;
    snprintf(out, cap, "%s", g_new);
    return true;
}

bool update_apply(char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (!g_self[0]) {
        if (err) snprintf(err, errlen, "cannot find the running .nro path");
        return false;
    }
    if (!path_exists(g_new)) {
        if (err) snprintf(err, errlen, "no uploaded update");
        return false;
    }
    char old[PATH_MAX + 64];
    snprintf(old, sizeof(old), "%s.old", g_self);
    unlink(old);
    if (rename(g_self, old)) {
        if (err) snprintf(err, errlen, "cannot move current .nro (%d)", errno);
        return false;
    }
    if (rename(g_new, g_self)) {
        rename(old, g_self);
        if (err) snprintf(err, errlen, "cannot move update in place (%d)", errno);
        return false;
    }
    unlink(old);
    return true;
}
