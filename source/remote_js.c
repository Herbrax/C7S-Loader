#include "remote_js.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "app.h"
#include "c7s.h"

/* ---- protocol constants (mirrors civswitch-ldn-nx installer.c) ---- */
#define LOADER_PORT       9026
#define MAX_SUBNET_HOSTS  512
#define SCAN_BATCH        32
#define SCAN_CONNECT_MS   250
#define LOADER_WAIT_NS    20000000000ULL   /* 20s scan budget */
#define SEND_TIMEOUT_NS   30000000000ULL
#define RESPONSE_CAPACITY 8192
#define SUCCESS_MARKER    "Executed successfully"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---- shared state ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile RjsPhase g_phase = RJS_IDLE;
static volatile bool g_busy = false;
static volatile bool g_stop = false;
static u32  g_loader_ip = 0;          /* network byte order */
static char g_loader_ip_str[24] = {0};
static char g_status[160] = "Idle";

static void set_status(RjsPhase ph, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_lock);
    g_phase = ph;
    snprintf(g_status, sizeof(g_status), "%s", buf);
    pthread_mutex_unlock(&g_lock);
}

/* ---- socket helpers (copied faithfully from installer.c) ---- */
static int set_nonblocking(int fd, bool enabled) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (enabled) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static int send_all(int fd, const char *data, size_t size) {
    while (size > 0) {
        ssize_t sent = send(fd, data, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        data += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int connect_ip(u32 ip, unsigned timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (set_nonblocking(fd, true) < 0) { close(fd); return -1; }

    struct sockaddr_in target = {0};
    target.sin_family = AF_INET;
    target.sin_port = htons(LOADER_PORT);
    target.sin_addr.s_addr = ip;
    int rc = connect(fd, (const struct sockaddr *)&target, sizeof(target));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }

    fd_set writes;
    FD_ZERO(&writes);
    FD_SET(fd, &writes);
    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    rc = select(fd + 1, NULL, &writes, NULL, &tv);
    if (rc <= 0 || !FD_ISSET(fd, &writes)) { close(fd); return -1; }

    int err = 0; socklen_t errsz = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errsz) < 0 || err) {
        close(fd); return -1;
    }
    if (set_nonblocking(fd, false) < 0) { close(fd); return -1; }
    return fd;
}

static void append_response(char *response, size_t *used,
                            const char *data, size_t size) {
    if (size >= RESPONSE_CAPACITY - 1) {
        data += size - (RESPONSE_CAPACITY - 1);
        size = RESPONSE_CAPACITY - 1;
        *used = 0;
    } else if (*used + size >= RESPONSE_CAPACITY) {
        size_t keep = RESPONSE_CAPACITY / 2;
        if (keep > *used) keep = *used;
        memmove(response, response + *used - keep, keep);
        *used = keep;
    }
    memcpy(response + *used, data, size);
    *used += size;
    response[*used] = 0;
}

/* Sends code, half-closes, then reads until success marker / error / timeout. */
static int run_code_on_socket(int fd, const char *code, size_t code_size,
                              const char *success, u64 timeout_ns) {
    struct timeval snd = { .tv_sec = 30, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) < 0)
        return -1;
    if (send_all(fd, code, code_size) < 0 || shutdown(fd, SHUT_WR) < 0)
        return -1;

    struct timeval rcv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    char response[RESPONSE_CAPACITY] = {0};
    size_t used = 0;
    u64 deadline = armGetSystemTick() + armNsToTicks(timeout_ns);
    int result = 1;

    while (armGetSystemTick() < deadline && !g_stop && appletMainLoop()) {
        char data[1024];
        ssize_t size = recv(fd, data, sizeof(data), 0);
        if (size > 0) {
            append_response(response, &used, data, (size_t)size);
            if (strstr(response, success) != NULL) { result = 0; break; }
            if (strstr(response, "E: ") || strstr(response, "EXCEPTION:") ||
                strstr(response, "Error in payload:") ||
                strstr(response, "ERROR in accept loop:")) {
                errno = EPROTO; result = -1; break;
            }
            continue;
        }
        if (size == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        result = -1; break;
    }
    if (result == 1) errno = ETIMEDOUT;
    return result;
}

static bool probe_loader_socket(int fd) {
    static const char probe[] = "1";
    if (set_nonblocking(fd, false) < 0) return false;
    return run_code_on_socket(fd, probe, sizeof(probe) - 1,
                              SUCCESS_MARKER, 3000000000ULL) == 0;
}

static bool connected_socket(int fd) {
    int err = 0; socklen_t sz = sizeof(err);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sz) == 0 && err == 0;
}

/* Returns loader IP (network order) or 0. */
static u32 scan_for_loader(u32 local_ip, u32 mask) {
    u32 local_host = ntohl(local_ip);
    u32 mask_host  = ntohl(mask);
    u64 count = (u64)(~mask_host) + 1;
    if (count < 4 || count > MAX_SUBNET_HOSTS) { errno = ENOTSUP; return 0; }
    u32 network = local_host & mask_host;

    u32 candidates[MAX_SUBNET_HOSTS];
    unsigned candidate_count = 0;
    for (u32 off = 1; off < count - 1; ++off) {
        u32 c = network + off;
        if (c != local_host) candidates[candidate_count++] = htonl(c);
    }

    u64 deadline = armGetSystemTick() + armNsToTicks(LOADER_WAIT_NS);
    unsigned pass = 0;
    while (armGetSystemTick() < deadline && !g_stop && appletMainLoop()) {
        pass++;
        set_status(RJS_SCANNING, "Scanning Wi-Fi for Switch 2 (pass %u)", pass);
        for (unsigned base = 0; base < candidate_count; base += SCAN_BATCH) {
            int sockets[SCAN_BATCH];
            unsigned batch = candidate_count - base;
            if (batch > SCAN_BATCH) batch = SCAN_BATCH;
            fd_set writes; FD_ZERO(&writes);
            int max_fd = -1;

            for (unsigned i = 0; i < batch; ++i) {
                sockets[i] = socket(AF_INET, SOCK_STREAM, 0);
                if (sockets[i] < 0 || set_nonblocking(sockets[i], true) < 0) {
                    if (sockets[i] >= 0) close(sockets[i]);
                    sockets[i] = -1; continue;
                }
                struct sockaddr_in t = {0};
                t.sin_family = AF_INET;
                t.sin_port = htons(LOADER_PORT);
                t.sin_addr.s_addr = candidates[base + i];
                int rc = connect(sockets[i], (const struct sockaddr *)&t, sizeof(t));
                if (rc < 0 && errno != EINPROGRESS) {
                    close(sockets[i]); sockets[i] = -1; continue;
                }
                FD_SET(sockets[i], &writes);
                if (sockets[i] > max_fd) max_fd = sockets[i];
            }

            if (max_fd >= 0) {
                struct timeval tv = { .tv_sec = 0, .tv_usec = SCAN_CONNECT_MS * 1000 };
                select(max_fd + 1, NULL, &writes, NULL, &tv);
            }
            for (unsigned i = 0; i < batch; ++i) {
                if (sockets[i] < 0) continue;
                bool ready = FD_ISSET(sockets[i], &writes) &&
                             connected_socket(sockets[i]);
                if (ready && probe_loader_socket(sockets[i])) {
                    u32 found = candidates[base + i];
                    close(sockets[i]);
                    for (unsigned j = i + 1; j < batch; ++j)
                        if (sockets[j] >= 0) close(sockets[j]);
                    return found;
                }
                close(sockets[i]);
            }
            if (armGetSystemTick() >= deadline || g_stop || !appletMainLoop()) break;
        }
        svcSleepThread(250000000LL);
    }
    errno = ETIMEDOUT;
    return 0;
}

/* Reads local Wi-Fi IP + subnet mask. Sockets are already up (web server). */
static bool local_ip_mask(u32 *ip, u32 *mask) {
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) return false;
    u32 gw = 0, d1 = 0, d2 = 0;
    bool ok = R_SUCCEEDED(nifmGetCurrentIpConfigInfo(ip, mask, &gw, &d1, &d2))
              && *ip != 0 && *mask != 0;
    nifmExit();
    return ok;
}

/* Finds (or reuses) a loader. Returns network-order IP or 0. */
static u32 ensure_loader(void) {
    if (g_loader_ip) return g_loader_ip;
    u32 ip = 0, mask = 0;
    if (!local_ip_mask(&ip, &mask)) {
        set_status(RJS_ERROR, "No Wi-Fi connection");
        return 0;
    }
    set_status(RJS_SCANNING, "Scanning Wi-Fi for Switch 2");
    u32 found = scan_for_loader(ip, mask);
    if (!found) {
        set_status(RJS_ERROR, g_stop ? "Scan cancelled"
                                     : "No RemoteJS loader found on Wi-Fi");
        return 0;
    }
    struct in_addr a; a.s_addr = found;
    pthread_mutex_lock(&g_lock);
    g_loader_ip = found;
    snprintf(g_loader_ip_str, sizeof(g_loader_ip_str), "%s", inet_ntoa(a));
    pthread_mutex_unlock(&g_lock);
    return found;
}

/* ---- worker threads ---- */
static void *scan_worker(void *arg) {
    (void)arg;
    g_stop = false;
    u32 ip = ensure_loader();
    if (ip) set_status(RJS_FOUND, "Loader at %s", g_loader_ip_str);
    g_busy = false;
    return NULL;
}

#define SAVE_TEMPLATE_CHUNK 400000

/* The save-installer .js expects window.__tpl to contain the base64 save
 * template.  The homebrew uploads it in chunks before applying the .js; do the
 * same here so sending Civ7SaveInstaller.js from the JS tab works. */
static int upload_template_data(u32 ip, const char *data, size_t size) {
    static const char reset[] = "log('STARTING INSTALLER');window.__tpl=null;'reset'";
    int fd = connect_ip(ip, 3000);
    if (fd < 0) return -1;
    int r = run_code_on_socket(fd, reset, sizeof(reset) - 1,
                               SUCCESS_MARKER, SEND_TIMEOUT_NS);
    close(fd);
    if (r != 0) return -1;

    const char prefix[] = "(window.__tpl=window.__tpl||[]).push('";
    const size_t plen = sizeof(prefix) - 1;
    int total = (int)((size + SAVE_TEMPLATE_CHUNK - 1) / SAVE_TEMPLATE_CHUNK);
    char *cmd = malloc(plen + SAVE_TEMPLATE_CHUNK + 96);
    if (!cmd) return -1;

    for (int i = 0; i < total; i++) {
        size_t off = (size_t)i * SAVE_TEMPLATE_CHUNK;
        size_t n = size - off;
        if (n > SAVE_TEMPLATE_CHUNK) n = SAVE_TEMPLATE_CHUNK;
        memcpy(cmd, prefix, plen);
        memcpy(cmd + plen, data + off, n);
        int tail = snprintf(cmd + plen + n, 96, "');'chunk %d/%d'", i + 1, total);
        if (tail <= 0) { free(cmd); return -1; }
        set_status(RJS_SENDING, "Uploading save template %d/%d", i + 1, total);
        fd = connect_ip(ip, 3000);
        if (fd < 0) { free(cmd); return -1; }
        r = run_code_on_socket(fd, cmd, plen + n + (size_t)tail,
                               SUCCESS_MARKER, SEND_TIMEOUT_NS);
        close(fd);
        if (r != 0) { free(cmd); return -1; }
    }
    free(cmd);
    return 0;
}

static int upload_save_template(u32 ip) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/Civ7SaveTemplate.b64", C7S_DIR) >= (int)sizeof(path))
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return -1; }
    char *data = malloc((size_t)size);
    if (!data) { fclose(f); return -1; }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(data); return -1; }
    int r = upload_template_data(ip, data, (size_t)size);
    free(data);
    return r;
}

/* Extract the JS and base64 template from the combined JS Payload .C7S. */
static bool extract_combined_js(const char *name, char **js_out, size_t *js_len,
                                char **tpl_out, size_t *tpl_len) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, name) >= (int)sizeof(path))
        return false;
    C7sPayload p;
    if (!c7s_parse_file(path, &p)) return false;
    if (!(p.types & C7S_TYPE_JS) || !p.data) { c7s_free(&p); return false; }
    char *data = p.data;
    char *jsm = strstr(data, "---JS---");
    char *tpm = jsm ? strstr(jsm + 8, "---TEMPLATE---") : NULL;
    char *endm = tpm ? strstr(tpm + 12, "---END---") : NULL;
    if (!jsm || !tpm || !endm) { c7s_free(&p); return false; }
    char *js = jsm + 8;
    while (*js == '\r' || *js == '\n') js++;
    char *js_end = tpm;
    while (js_end > js && (js_end[-1] == '\r' || js_end[-1] == '\n')) js_end--;
    char *tpl = tpm + 12;
    while (*tpl == '\r' || *tpl == '\n' || *tpl == ' ' || *tpl == '\t') tpl++;
    size_t tl = (size_t)(endm - tpl);
    char *tpl_copy = malloc(tl + 1);
    if (!tpl_copy) { c7s_free(&p); return false; }
    size_t tw = 0;
    for (size_t i = 0; i < tl; i++) {
        char c = tpl[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        tpl_copy[tw++] = c;
    }
    tpl_copy[tw] = 0;
    size_t jl = (size_t)(js_end - js);
    char *js_copy = malloc(jl + 1);
    if (!js_copy) { free(tpl_copy); c7s_free(&p); return false; }
    memcpy(js_copy, js, jl);
    js_copy[jl] = 0;
    c7s_free(&p);
    *js_out = js_copy;
    *js_len = jl;
    *tpl_out = tpl_copy;
    *tpl_len = tw;
    return true;
}

typedef struct { char name[128]; } SendArgs;

static void *send_worker(void *argp) {
    SendArgs *a = argp;
    g_stop = false;

    u32 ip = ensure_loader();
    if (!ip) { free(a); g_busy = false; return NULL; }

    bool combined = false;
    for (const char *p = a->name; *p; p++)
        if (p[0] == '.' && strcasecmp(p, ".C7S") == 0) { combined = true; break; }
    if (combined) {
        char *js = NULL, *tpl = NULL;
        size_t jl = 0, tl = 0;
        if (!extract_combined_js(a->name, &js, &jl, &tpl, &tl)) {
            set_status(RJS_ERROR, "Cannot parse %s", a->name);
            free(a); g_busy = false; return NULL;
        }
        if (upload_template_data(ip, tpl, tl) != 0) {
            set_status(RJS_ERROR, "Save template upload failed");
            free(js); free(tpl); free(a); g_busy = false; return NULL;
        }
        set_status(RJS_SENDING, "Applying %s", a->name);
        int cfd = connect_ip(ip, 3000);
        int cr = cfd < 0 ? -1 : run_code_on_socket(cfd, js, jl, "INSTALL DONE", 300000000000ULL);
        if (cfd >= 0) close(cfd);
        free(js); free(tpl);
        if (cr == 0) set_status(RJS_DONE, "Installed %s", a->name);
        else set_status(RJS_ERROR, "%s did not report INSTALL DONE", a->name);
        free(a); g_busy = false; return NULL;
    }

    bool save_installer = strcasecmp(a->name, "Civ7SaveInstaller.js") == 0;
    if (save_installer) {
        if (upload_save_template(ip) != 0) {
            set_status(RJS_ERROR, "Save template upload failed");
            free(a);
            g_busy = false;
            return NULL;
        }
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, a->name) >= (int)sizeof(path)) {
        set_status(RJS_ERROR, "Bad file name");
        free(a); g_busy = false; return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_status(RJS_ERROR, "Cannot open %s", a->name);
        free(a); g_busy = false; return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); set_status(RJS_ERROR, "%s is empty", a->name);
                   free(a); g_busy = false; return NULL; }
    char *code = malloc((size_t)sz);
    if (!code) { fclose(f); set_status(RJS_ERROR, "Out of memory");
                 free(a); g_busy = false; return NULL; }
    size_t rd = fread(code, 1, (size_t)sz, f);
    fclose(f);

    set_status(RJS_SENDING, "Sending %s to %s", a->name, g_loader_ip_str);
    int fd = connect_ip(ip, 3000);
    if (fd < 0) {
        set_status(RJS_ERROR, "Lost loader at %s", g_loader_ip_str);
        pthread_mutex_lock(&g_lock); g_loader_ip = 0; pthread_mutex_unlock(&g_lock);
        free(code); free(a); g_busy = false; return NULL;
    }
    const char *success = save_installer ? "INSTALL DONE" : SUCCESS_MARKER;
    u64 timeout = save_installer ? 300000000000ULL : SEND_TIMEOUT_NS;
    int r = run_code_on_socket(fd, code, rd, success, timeout);
    close(fd);
    free(code);

    if (r == 0)      set_status(RJS_DONE, "Sent %s - executed OK", a->name);
    else if (r < 0 && errno == EPROTO)
                     set_status(RJS_ERROR, "%s threw an error on the loader", a->name);
    else             set_status(RJS_ERROR, "No reply from loader (timeout)");
    free(a);
    g_busy = false;
    return NULL;
}

static bool spawn(void *(*fn)(void *), void *arg) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc == 0) { pthread_detach(th); return true; }
    return false;
}

/* ---- public API ---- */
void remote_js_init(void) {
    g_phase = RJS_IDLE;
    g_busy = false;
    g_stop = false;
    g_loader_ip = 0;
    g_loader_ip_str[0] = '\0';
    snprintf(g_status, sizeof(g_status), "Idle");
}

void remote_js_shutdown(void) {
    g_stop = true;
}

void remote_js_scan(void) {
    if (g_busy) return;
    g_busy = true;
    if (!spawn(scan_worker, NULL)) {
        g_busy = false;
        set_status(RJS_ERROR, "Could not start scan");
    }
}

void remote_js_send(const char *filename) {
    if (!filename || !*filename) return;
    if (g_busy) return;
    SendArgs *a = malloc(sizeof(*a));
    if (!a) { set_status(RJS_ERROR, "Out of memory"); return; }
    snprintf(a->name, sizeof(a->name), "%s", filename);
    g_busy = true;
    if (!spawn(send_worker, a)) {
        free(a);
        g_busy = false;
        set_status(RJS_ERROR, "Could not start send");
    }
}

void remote_js_reset(void) {
    pthread_mutex_lock(&g_lock);
    g_loader_ip = 0;
    g_loader_ip_str[0] = '\0';
    if (!g_busy) { g_phase = RJS_IDLE; snprintf(g_status, sizeof(g_status), "Idle"); }
    pthread_mutex_unlock(&g_lock);
}

RjsPhase remote_js_phase(void) { return g_phase; }

const char *remote_js_phase_name(void) {
    switch (g_phase) {
        case RJS_SCANNING: return "SCANNING";
        case RJS_FOUND:    return "LOADER FOUND";
        case RJS_SENDING:  return "SENDING";
        case RJS_DONE:     return "DONE";
        case RJS_ERROR:    return "ERROR";
        default:           return "IDLE";
    }
}

const char *remote_js_status(void) { return g_status; }
bool remote_js_busy(void) { return g_busy; }
bool remote_js_have_loader(void) { return g_loader_ip != 0; }
const char *remote_js_loader_ip_str(void) { return g_loader_ip_str; }

static bool has_js_ext(const char *name) {
    size_t n = strlen(name);
    return n > 3 && strcasecmp(name + n - 3, ".js") == 0;
}

int remote_js_list(char out[][128], int max) {
    if (!out || max <= 0) return 0;
    DIR *d = opendir(C7S_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        bool is_js = has_js_ext(e->d_name);
        bool is_combined = false;
        char path[PATH_MAX];
        if (!is_js) {
            size_t len = strlen(e->d_name);
            if (len <= 4 || strcasecmp(e->d_name + len - 4, ".c7s") != 0) continue;
            if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, e->d_name) >= (int)sizeof(path)) continue;
            C7sPayload p;
            if (c7s_parse_file(path, &p)) {
                is_combined = (p.types & C7S_TYPE_JS) != 0;
                c7s_free(&p);
            }
            if (!is_combined) continue;
        } else {
            if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, e->d_name) >= (int)sizeof(path)) continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n], 128, "%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}
