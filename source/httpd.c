#include "httpd.h"

#include <microhttpd.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "c7s.h"
#include "debuglog.h"
#include "ldn_route.h"
#include "remote_js.h"
#include "live.h"
#include "update.h"
#include "settings.h"
#include "usb_composite.h"
#include "web_assets.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define REQUEST_BODY_MAX 4096

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sb_t;

static void sb_reserve(sb_t *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return;
    b->data = p;
    b->cap = cap;
}

static void sb_add(sb_t *b, const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    sb_reserve(b, n);
    if (!b->data) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_addf(sb_t *b, const char *fmt, ...) {
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    sb_add(b, tmp);
}

static void sb_json(sb_t *b, const char *s) {
    sb_add(b, "\"");
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            unsigned char c = *p;
            if (c == '"') sb_add(b, "\\\"");
            else if (c == '\\') sb_add(b, "\\\\");
            else if (c == '\n') sb_add(b, "\\n");
            else if (c == '\r') sb_add(b, "\\r");
            else if (c == '\t') sb_add(b, "\\t");
            else if (c < 0x20) sb_addf(b, "\\u%04x", c);
            else {
                char one[2] = {(char)c, 0};
                sb_add(b, one);
            }
        }
    }
    sb_add(b, "\"");
}

static void sb_free(sb_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static enum MHD_Result reply_json(struct MHD_Connection *conn, unsigned int status, sb_t *b) {
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(b->len, b->data ? b->data : (char *)"", MHD_RESPMEM_MUST_COPY);
    if (!resp) {
        sb_free(b);
        return MHD_NO;
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    sb_free(b);
    return ret;
}

static enum MHD_Result reply_error(struct MHD_Connection *conn, unsigned int status, const char *msg) {
    sb_t b = {0};
    sb_add(&b, "{\"ok\":false,\"error\":");
    sb_json(&b, msg);
    sb_add(&b, "}");
    return reply_json(conn, status, &b);
}

static enum MHD_Result reply_asset(struct MHD_Connection *conn, unsigned int status,
                                   const char *ctype, const unsigned char *data, unsigned int len) {
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, (void *)data, MHD_RESPMEM_PERSISTENT);
    if (!resp) return MHD_NO;
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, ctype);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static void add_meta(sb_t *b, const C7sPayload *p, long long size) {
    sb_add(b, ",\"size\":");
    sb_addf(b, "%lld", size);
    sb_add(b, ",\"header\":");
    sb_add(b, p->header_valid ? "true" : "false");
    sb_add(b, ",\"title\":");
    sb_json(b, p->header_valid ? p->title : "");
    sb_add(b, ",\"desc\":");
    sb_json(b, p->desc);
    sb_add(b, ",\"lines\":");
    sb_addf(b, "%d", p->line_count);
    /* type flags */
    sb_add(b, ",\"text\":");
    sb_add(b, c7s_is_text(p) ? "true" : "false");
    sb_add(b, ",\"ldn\":");
    sb_add(b, c7s_is_ldn(p) ? "true" : "false");
    sb_add(b, ",\"script\":");
    sb_add(b, c7s_is_script(p) ? "true" : "false");
}

typedef struct {
    int upload;
    int is_update;
    size_t max;
    int fd;
    int err;
    unsigned long long written;
    char tmp[PATH_MAX];
    char final[PATH_MAX];
    char *body;
    size_t size;
    bool too_large;
} req_t;

static void upload_open(struct MHD_Connection *conn, req_t *ctx) {
    const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    ctx->upload = 1;
    ctx->is_update = 0;
    ctx->max = C7S_MAX_BYTES;
    ctx->fd = -1;
    if (!c7s_upload_name_ok(name)) {
        ctx->err = EINVAL;
        return;
    }
    if (snprintf(ctx->final, sizeof(ctx->final), "%s/%s", C7S_DIR, name) >= (int)sizeof(ctx->final)) {
        ctx->err = ENAMETOOLONG;
        return;
    }
    if (snprintf(ctx->tmp, sizeof(ctx->tmp), "%s.upload", ctx->final) >= (int)sizeof(ctx->tmp)) {
        ctx->err = ENAMETOOLONG;
        return;
    }
    ctx->fd = open(ctx->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (ctx->fd < 0) ctx->err = errno;
}

static void upload_write(req_t *ctx, const char *data, size_t n) {
    if (!n || ctx->err || ctx->fd < 0) return;
    if (ctx->written + n > ctx->max) {
        ctx->err = EFBIG;
        return;
    }
    while (n) {
        ssize_t w = write(ctx->fd, data, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            ctx->err = w < 0 ? errno : ENOSPC;
            return;
        }
        data += w;
        n -= (size_t)w;
        ctx->written += (unsigned long long)w;
    }
}

static void update_open(struct MHD_Connection *conn, req_t *ctx) {
    (void)conn;
    ctx->upload = 1;
    ctx->is_update = 1;
    ctx->max = 64u * 1024u * 1024u;
    ctx->fd = -1;
    ctx->tmp[0] = '\0';
    if (!update_new_path(ctx->final, sizeof(ctx->final))) {
        ctx->err = EINVAL;
        return;
    }
    ctx->fd = open(ctx->final, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (ctx->fd < 0) ctx->err = errno;
}

static bool nro_magic_ok(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char hdr[0x14];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    return n >= sizeof(hdr) && memcmp(hdr + 0x10, "NRO0", 4) == 0;
}

static enum MHD_Result upload_finish(struct MHD_Connection *conn, req_t *ctx) {
    if (ctx->fd >= 0) {
        if (close(ctx->fd) && !ctx->err) ctx->err = errno;
        ctx->fd = -1;
    }
    if (ctx->is_update) {
        if (!ctx->err && ctx->written == 0) ctx->err = EINVAL;
        if (!ctx->err && !nro_magic_ok(ctx->final)) ctx->err = EINVAL;
        if (ctx->err) {
            if (ctx->final[0]) unlink(ctx->final);
            return reply_error(conn, 400, "not a valid .nro update");
        }
        app_request_update();
        sb_t b = {0};
        sb_add(&b, "{\"ok\":true,\"size\":");
        sb_addf(&b, "%llu", ctx->written);
        sb_add(&b, ",\"path\":");
        sb_json(&b, ctx->final);
        sb_add(&b, ",\"note\":\"app will close; reopen from hbmenu\"}");
        return reply_json(conn, 200, &b);
    }
    if (!ctx->err && ctx->written == 0) ctx->err = EINVAL;
    if (!ctx->err && rename(ctx->tmp, ctx->final)) ctx->err = errno;
    if (ctx->err) {
        if (ctx->tmp[0]) unlink(ctx->tmp);
        return reply_error(conn, ctx->err == EINVAL ? 400 : 500, strerror(ctx->err));
    }

    const char *fname = strrchr(ctx->final, '/') ? strrchr(ctx->final, '/') + 1 : ctx->final;
    const char *fdot = strrchr(fname, '.');
    if (fdot && strcasecmp(fdot, ".js") == 0) {
        /* Standalone JS payload: no C7S header to parse. */
        sb_t jb = {0};
        sb_add(&jb, "{\"ok\":true,\"js\":true,\"name\":");
        sb_json(&jb, fname);
        sb_add(&jb, ",\"size\":");
        sb_addf(&jb, "%llu", ctx->written);
        sb_add(&jb, "}");
        return reply_json(conn, 200, &jb);
    }

    C7sPayload p;
    bool ok = c7s_parse_file(ctx->final, &p);
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"name\":");
    sb_json(&b, fname);
    if (ok) {
        add_meta(&b, &p, (long long)ctx->written);
        c7s_free(&p);
    } else {
        sb_add(&b, ",\"size\":");
        sb_addf(&b, "%llu", ctx->written);
        sb_add(&b, ",\"header\":false,\"title\":\"\",\"declared\":0,\"lines\":0,\"mismatch\":false,\"error\":");
        sb_json(&b, p.error);
    }
    sb_add(&b, "}");
    const char *base = strrchr(ctx->final, '/');
    app_set_selected(base ? base + 1 : ctx->final);
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_state(struct MHD_Connection *conn) {
    C7sAppSnapshot s;
    app_snapshot(&s);
    sb_t b = {0};
    sb_add(&b, "{\"running\":");
    sb_add(&b, s.running ? "true" : "false");
    sb_add(&b, ",\"finished\":");
    sb_add(&b, s.finished ? "true" : "false");
    sb_add(&b, ",\"ok\":");
    sb_add(&b, s.ok ? "true" : "false");
    sb_add(&b, ",\"step\":");
    sb_addf(&b, "%d", s.step);
    sb_add(&b, ",\"total\":");
    sb_addf(&b, "%d", s.total);
    sb_add(&b, ",\"selected\":");
    sb_json(&b, s.selected);
    sb_add(&b, ",\"message\":");
    sb_json(&b, s.message);
    sb_add(&b, ",\"port\":");
    sb_addf(&b, "%u", (unsigned)s.port);
    sb_add(&b, ",\"ip\":");
    sb_json(&b, s.ip);
    const UsbCompositeStatus *u = usb_composite_status();
    sb_add(&b, ",\"usb\":{\"initialized\":");
    sb_add(&b, u->initialized ? "true" : "false");
    sb_add(&b, ",\"pad_ready\":");
    sb_add(&b, u->pad_ready ? "true" : "false");
    sb_add(&b, ",\"kbd_ready\":");
    sb_add(&b, u->kbd_ready ? "true" : "false");
    sb_add(&b, ",\"state\":");
    sb_addf(&b, "%d", (int)u->state);
    sb_add(&b, ",\"error\":");
    sb_addf(&b, "%u", (unsigned)u->error);
    sb_add(&b, ",\"stage\":");
    sb_json(&b, u->stage);
    sb_add(&b, ",\"reports_failed\":");
    sb_addf(&b, "%u", (unsigned)u->reports_failed);
    sb_add(&b, "}");
    sb_add(&b, "}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_files(struct MHD_Connection *conn) {
    sb_t b = {0};
    sb_add(&b, "[");
    DIR *d = opendir(C7S_DIR);
    if (d) {
        bool first = true;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!c7s_name_ok(e->d_name)) continue;
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, e->d_name) >= (int)sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            C7sPayload p;
            bool ok = c7s_parse_file(path, &p);
            sb_add(&b, first ? "{\"name\":" : ",{\"name\":");
            sb_json(&b, e->d_name);
            if (ok) {
                add_meta(&b, &p, (long long)st.st_size);
                c7s_free(&p);
            } else {
                sb_add(&b, ",\"size\":");
                sb_addf(&b, "%lld", (long long)st.st_size);
                sb_add(&b, ",\"header\":false,\"title\":\"\",\"declared\":0,\"lines\":0,\"mismatch\":false,\"error\":");
                sb_json(&b, p.error);
            }
            sb_add(&b, "}");
            first = false;
        }
        closedir(d);
    }
    sb_add(&b, "]");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_start(struct MHD_Connection *conn) {
    C7sAppSnapshot s;
    app_snapshot(&s);
    if (s.running) return reply_error(conn, 409, "already running");

    const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    if (!name || !name[0]) name = s.selected;
    if (!c7s_name_ok(name)) return reply_error(conn, 400, "invalid file name");

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path)) {
        return reply_error(conn, 400, "path too long");
    }
    if (access(path, R_OK) != 0) return reply_error(conn, 404, "file not found");

    app_request_start(name);
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"name\":");
    sb_json(&b, name);
    sb_add(&b, "}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_cancel(struct MHD_Connection *conn) {
    app_request_cancel();
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_settings(struct MHD_Connection *conn) {
    const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "port");
    if (!v) return reply_error(conn, 400, "missing port");
    int port = atoi(v);
    if (port < 1 || port > 65535) return reply_error(conn, 400, "port must be 1..65535");
    settings_save_port((unsigned short)port);
    app_request_restart((unsigned short)port);
    sb_t b = {0};
    sb_addf(&b, "{\"ok\":true,\"port\":%d}", port);
    return reply_json(conn, 200, &b);
}

static bool read_text_file(const char *path, char *buf, size_t cap, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!cap) return false;
    buf[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return true;
}

static enum MHD_Result api_log(struct MHD_Connection *conn) {
    const char *which = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "which");
    char text[16385];
    if (which && !strcmp(which, "run")) {
        dbg_snapshot_run(text, sizeof(text));
    } else if (which && (!strcmp(which, "exit") || !strcmp(which, "usbprev") || !strcmp(which, "runprev"))) {
        const char *path = "sdmc:/switch/C7S-Loader/exit.log";
        if (!strcmp(which, "usbprev")) path = "sdmc:/switch/C7S-Loader/usb-prev.log";
        else if (!strcmp(which, "runprev")) path = "sdmc:/switch/C7S-Loader/run-prev.log";
        read_text_file(path, text, sizeof(text), NULL);
    } else {
        which = "usb";
        dbg_snapshot_usb(text, sizeof(text));
    }
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"which\":");
    sb_json(&b, which);
    sb_add(&b, ",\"text\":");
    sb_json(&b, text);
    sb_add(&b, "}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_debug(struct MHD_Connection *conn) {
    C7sAppSnapshot s;
    app_snapshot(&s);
    const UsbCompositeStatus *u = usb_composite_status();
    sb_t b = {0};
    sb_add(&b, "{\"app\":{\"running\":");
    sb_add(&b, s.running ? "true" : "false");
    sb_add(&b, ",\"finished\":");
    sb_add(&b, s.finished ? "true" : "false");
    sb_add(&b, ",\"ok\":");
    sb_add(&b, s.ok ? "true" : "false");
    sb_add(&b, ",\"step\":");
    sb_addf(&b, "%d", s.step);
    sb_add(&b, ",\"total\":");
    sb_addf(&b, "%d", s.total);
    sb_add(&b, ",\"selected\":");
    sb_json(&b, s.selected);
    sb_add(&b, ",\"message\":");
    sb_json(&b, s.message);
    sb_add(&b, "},\"usb\":{\"backend\":");
    sb_json(&b, usb_backend_name());
    sb_add(&b, ",\"initialized\":");
    sb_add(&b, u->initialized ? "true" : "false");
    sb_add(&b, ",\"configured\":");
    sb_add(&b, u->configured ? "true" : "false");
    sb_add(&b, ",\"pad_ready\":");
    sb_add(&b, u->pad_ready ? "true" : "false");
    sb_add(&b, ",\"kbd_ready\":");
    sb_add(&b, u->kbd_ready ? "true" : "false");
    sb_add(&b, ",\"pad_pending\":");
    sb_add(&b, u->pad_pending ? "true" : "false");
    sb_add(&b, ",\"kbd_pending\":");
    sb_add(&b, u->kbd_pending ? "true" : "false");
    sb_add(&b, ",\"state\":");
    sb_addf(&b, "%d", (int)u->state);
    sb_add(&b, ",\"error\":");
    sb_addf(&b, "%u", (unsigned)u->error);
    sb_add(&b, ",\"stage\":");
    sb_json(&b, u->stage);
    sb_add(&b, ",\"pad_sent\":");
    sb_addf(&b, "%u", (unsigned)u->pad_sent);
    sb_add(&b, ",\"kbd_sent\":");
    sb_addf(&b, "%u", (unsigned)u->kbd_sent);
    sb_add(&b, ",\"reports_failed\":");
    sb_addf(&b, "%u", (unsigned)u->reports_failed);
    sb_add(&b, ",\"bus_resets\":");
    sb_addf(&b, "%u", (unsigned)u->bus_resets);
    sb_add(&b, ",\"leds\":");
    sb_addf(&b, "%u", (unsigned)u->leds);
    sb_add(&b, "}}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_live(struct MHD_Connection *conn) {
    const char *on = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "on");
    const char *release = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "release");
    if (on) {
        live_set_enabled(!strcmp(on, "1") || !strcmp(on, "true"));
    } else if (release) {
        live_release_all();
    }
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"enabled\":");
    sb_add(&b, live_enabled() ? "true" : "false");
    sb_add(&b, "}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_live_key(struct MHD_Connection *conn) {
    C7sAppSnapshot s;
    app_snapshot(&s);
    if (s.running) return reply_error(conn, 409, "payload running");
    if (!live_enabled()) return reply_error(conn, 409, "live input is off");
    if (!usb_keyboard_ready()) return reply_error(conn, 409, "keyboard not ready");

    const char *mv = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "mod");
    const char *kv = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "key");
    if (!mv || !kv) return reply_error(conn, 400, "missing mod/key");
    unsigned long mod = strtoul(mv, NULL, 0);
    unsigned long key = strtoul(kv, NULL, 0);
    if (mod > 255 || key > 255) return reply_error(conn, 400, "invalid key");

    live_set_key((uint8_t)mod, (uint8_t)key);
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_live_pad(struct MHD_Connection *conn) {
    C7sAppSnapshot s;
    app_snapshot(&s);
    if (s.running) return reply_error(conn, 409, "payload running");
    if (!live_enabled()) return reply_error(conn, 409, "live input is off");
    if (!usb_gamepad_ready()) return reply_error(conn, 409, "gamepad not ready");

    const char *bv = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "buttons");
    const char *hv = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "hat");
    if (!bv || !hv) return reply_error(conn, 400, "missing buttons/hat");
    unsigned long buttons = strtoul(bv, NULL, 0);
    unsigned long hat = strtoul(hv, NULL, 0);
    if (buttons > 0xFFFF || hat > 255) return reply_error(conn, 400, "invalid pad");

    C7sPadReport pad;
    memset(&pad, 0, sizeof(pad));
    pad.buttons = (uint16_t)buttons;
    pad.hat = (uint8_t)hat;
    pad.lx = SWITCH_STICK_MID;
    pad.ly = SWITCH_STICK_MID;
    pad.rx = SWITCH_STICK_MID;
    pad.ry = SWITCH_STICK_MID;

    static const char *axes[4] = {"lx", "ly", "rx", "ry"};
    uint8_t *dst[4] = {&pad.lx, &pad.ly, &pad.rx, &pad.ry};
    for (int i = 0; i < 4; i++) {
        const char *v = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, axes[i]);
        if (!v) continue;
        unsigned long x = strtoul(v, NULL, 0);
        if (x > 255) return reply_error(conn, 400, "invalid axis");
        *dst[i] = (uint8_t)x;
    }

    live_set_pad(&pad);
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_download(struct MHD_Connection *conn) {
    const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
    if (!name || !c7s_name_ok(name)) return reply_error(conn, 400, "invalid file name");

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path)) {
        return reply_error(conn, 400, "path too long");
    }
    FILE *f = fopen(path, "rb");
    if (!f) return reply_error(conn, 404, "not found");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return reply_error(conn, 500, "cannot seek");
    }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > 4u * 1024u * 1024u) {
        fclose(f);
        return reply_error(conn, 400, "file too large");
    }
    rewind(f);
    char *data = malloc((size_t)size ? (size_t)size : 1);
    if (!data) {
        fclose(f);
        return reply_error(conn, 500, "out of memory");
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return reply_error(conn, 500, "short read");
    }

    struct MHD_Response *resp =
        MHD_create_response_from_buffer((size_t)size, data, MHD_RESPMEM_MUST_FREE);
    if (!resp) {
        free(data);
        return MHD_NO;
    }
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/octet-stream");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_DISPOSITION, disposition);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-store");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");
    enum MHD_Result ret = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result api_ldn(struct MHD_Connection *conn) {
    // Actions mirror the console flow: scan -> join -> send -> stop.
    const char *action = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "action");
    const char *start = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "start");
    if (action) {
        if (!strcmp(action, "scan")) {
            ldn_route_scan();
        } else if (!strcmp(action, "join")) {
            const char *idx = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "index");
            ldn_route_join(idx ? atoi(idx) : 0);
        } else if (!strcmp(action, "send")) {
            ldn_route_send_loader();
        } else if (!strcmp(action, "test")) {
            ldn_route_send_test();
        } else if (!strcmp(action, "stop")) {
            ldn_route_stop();
        }
    } else if (start && (!strcmp(start, "1") || !strcmp(start, "true"))) {
        ldn_route_scan();
    }

    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"active\":");
    sb_add(&b, ldn_route_active() ? "true" : "false");
    sb_add(&b, ",\"connected\":");
    sb_add(&b, ldn_route_connected() ? "true" : "false");
    sb_add(&b, ",\"status\":");
    sb_json(&b, ldn_route_status());
    sb_add(&b, ",\"lobbies\":[");
    int n = ldn_route_scan_count();
    for (int i = 0; i < n; i++) {
        const LdnLobbyInfo *li = ldn_route_scan_item(i);
        if (!li) break;
        if (i) sb_add(&b, ",");
        sb_add(&b, "{\"ssid\":");
        sb_json(&b, li->ssid[0] ? li->ssid : "(hidden)");
        sb_addf(&b, ",\"players\":%d,\"max\":%d,\"channel\":%d,\"link\":%d,\"joinable\":%s,\"problem\":",
                (int)li->node_count, (int)li->node_count_max, (int)li->channel,
                (int)li->link_level, li->joinable ? "true" : "false");
        sb_json(&b, li->problem);
        sb_add(&b, "}");
    }
    sb_add(&b, "]}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_js(struct MHD_Connection *conn) {
    /* RemoteJS: scan the Wi-Fi for a loader on :9026, then push a .js file. */
    const char *action = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "action");
    if (action) {
        if (!strcmp(action, "scan")) {
            remote_js_scan();
        } else if (!strcmp(action, "send")) {
            const char *name = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "name");
            if (name) remote_js_send(name);
        } else if (!strcmp(action, "reset")) {
            remote_js_reset();
        }
    }

    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"phase\":");
    sb_json(&b, remote_js_phase_name());
    sb_add(&b, ",\"status\":");
    sb_json(&b, remote_js_status());
    sb_add(&b, ",\"busy\":");
    sb_add(&b, remote_js_busy() ? "true" : "false");
    sb_add(&b, ",\"loader\":");
    sb_json(&b, remote_js_loader_ip_str());
    sb_add(&b, ",\"files\":[");
    char names[64][128];
    int n = remote_js_list(names, 64);
    for (int i = 0; i < n; i++) {
        if (i) sb_add(&b, ",");
        sb_json(&b, names[i]);
    }
    sb_add(&b, "]}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result api_ktest(struct MHD_Connection *conn) {
    app_request_ktest();
    sb_t b = {0};
    sb_add(&b, "{\"ok\":true,\"message\":\"keyboard test queued (5s countdown)\"}");
    return reply_json(conn, 200, &b);
}

static enum MHD_Result serve_asset(struct MHD_Connection *conn, const char *url) {
    if (!strcmp(url, "/") || !strcmp(url, "/index.html")) {
        return reply_asset(conn, 200, "text/html; charset=utf-8", web_index_html, web_index_html_len);
    }
    if (!strcmp(url, "/app.css")) {
        return reply_asset(conn, 200, "text/css; charset=utf-8", web_app_css, web_app_css_len);
    }
    if (!strcmp(url, "/app.js")) {
        return reply_asset(conn, 200, "application/javascript; charset=utf-8", web_app_js, web_app_js_len);
    }
    if (!strcmp(url, "/favicon.ico")) {
        return reply_error(conn, 404, "not found");
    }
    return reply_error(conn, 404, "not found");
}

static enum MHD_Result dispatch(struct MHD_Connection *conn, const char *url, const char *method) {
    bool get = !strcmp(method, MHD_HTTP_METHOD_GET) || !strcmp(method, MHD_HTTP_METHOD_HEAD);
    bool post = !strcmp(method, MHD_HTTP_METHOD_POST);

    if (get && !strcmp(url, "/api/state")) return api_state(conn);
    if (get && !strcmp(url, "/api/debug")) return api_debug(conn);
    if (get && !strcmp(url, "/api/log")) return api_log(conn);
    if (post && !strcmp(url, "/api/ktest")) return api_ktest(conn);
    if (get && !strcmp(url, "/api/files")) return api_files(conn);
    if (get && !strcmp(url, "/api/download")) return api_download(conn);
    if (get && !strcmp(url, "/api/ldn")) return api_ldn(conn);
    if (post && !strcmp(url, "/api/ldn")) return api_ldn(conn);
    if (get && !strcmp(url, "/api/js")) return api_js(conn);
    if (post && !strcmp(url, "/api/js")) return api_js(conn);
    if (post && !strcmp(url, "/api/start")) return api_start(conn);
    if (post && !strcmp(url, "/api/cancel")) return api_cancel(conn);
    if (post && !strcmp(url, "/api/settings")) return api_settings(conn);
    if ((get || post) && !strcmp(url, "/api/live")) return api_live(conn);
    if (post && !strcmp(url, "/api/live/key")) return api_live_key(conn);
    if (post && !strcmp(url, "/api/live/pad")) return api_live_pad(conn);

    if (!strncmp(url, "/api/", 5)) {
        return reply_error(conn, get || post ? 404 : 405, "unknown endpoint");
    }
    if (get) return serve_asset(conn, url);
    return reply_error(conn, 405, "method not allowed");
}

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn, const char *url,
                                  const char *method, const char *version, const char *upload_data,
                                  size_t *upload_data_size, void **con_cls) {
    (void)cls;
    (void)version;
    req_t *ctx = *con_cls;

    if (strcmp(method, MHD_HTTP_METHOD_GET) && strcmp(method, MHD_HTTP_METHOD_HEAD) &&
        strcmp(method, MHD_HTTP_METHOD_POST)) {
        return MHD_NO;
    }
    if (!ctx) {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx) return MHD_NO;
        ctx->fd = -1;
        *con_cls = ctx;
        if (!strcmp(url, "/api/update") && !strcmp(method, MHD_HTTP_METHOD_POST)) update_open(conn, ctx);
        else if (!strcmp(url, "/api/upload") && !strcmp(method, MHD_HTTP_METHOD_POST)) upload_open(conn, ctx);
        return MHD_YES;
    }
    if (ctx->upload) {
        if (*upload_data_size) {
            upload_write(ctx, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }
        return upload_finish(conn, ctx);
    }
    if (*upload_data_size) {
        size_t n = *upload_data_size;
        if (n > REQUEST_BODY_MAX - ctx->size) {
            ctx->too_large = true;
        } else if (!ctx->too_large) {
            char *body = realloc(ctx->body, ctx->size + n + 1);
            if (!body) return MHD_NO;
            ctx->body = body;
            memcpy(ctx->body + ctx->size, upload_data, n);
            ctx->size += n;
            ctx->body[ctx->size] = 0;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }
    return dispatch(conn, url, method);
}

static void on_completed(void *cls, struct MHD_Connection *conn, void **con_cls,
                         enum MHD_RequestTerminationCode toe) {
    (void)cls;
    (void)conn;
    (void)toe;
    req_t *ctx = *con_cls;
    if (!ctx) return;
    if (ctx->fd >= 0) {
        close(ctx->fd);
        if (ctx->is_update) {
            if (ctx->final[0]) unlink(ctx->final);
        } else if (ctx->tmp[0]) {
            unlink(ctx->tmp);
        }
    }
    free(ctx->body);
    free(ctx);
    *con_cls = NULL;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static volatile int g_stop;
static volatile int g_failed;
static volatile int g_running;
static unsigned short g_port;

static void *serve(void *arg) {
    (void)arg;
    struct MHD_Daemon *daemon =
        MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD |
                             MHD_USE_POLL | MHD_USE_ERROR_LOG,
                         g_port, NULL, NULL, &on_request, NULL,
                         MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)5,
                         MHD_OPTION_CONNECTION_LIMIT, (unsigned int)16,
                         MHD_OPTION_NOTIFY_COMPLETED, &on_completed, NULL,
                         MHD_OPTION_THREAD_STACK_SIZE, (size_t)(160 * 1024),
                         MHD_OPTION_END);
    if (!daemon) {
        g_failed = 1;
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    g_running = 1;
    pthread_mutex_unlock(&g_lock);
    while (!g_stop) usleep(50000);
    MHD_stop_daemon(daemon);
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int httpd_start(unsigned short port) {
    pthread_mutex_lock(&g_lock);
    if (g_running) {
        pthread_mutex_unlock(&g_lock);
        httpd_stop();
        pthread_mutex_lock(&g_lock);
    }
    g_stop = 0;
    g_failed = 0;
    g_running = 0;
    g_port = port;
    int rc = pthread_create(&g_thread, NULL, serve, NULL);
    pthread_mutex_unlock(&g_lock);
    if (rc) return -1;
    pthread_detach(g_thread);
    for (int i = 0; i < 300; i++) {
        if (g_running) return 0;
        if (g_failed) return -1;
        usleep(10000);
    }
    return -1;
}

void httpd_request_stop(void) {
    g_stop = 1;
}

void httpd_stop(void) {
    app_exit_trace("httpd_stop: begin running=%d", g_running);
    g_stop = 1;
    for (int i = 0; i < 400 && g_running; i++) usleep(10000);
    app_exit_trace("httpd_stop: end running=%d", g_running);
}

bool httpd_running(void) { return g_running != 0; }
unsigned short httpd_port(void) { return g_port; }
