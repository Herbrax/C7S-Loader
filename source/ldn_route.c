#include "ldn_route.h"

#include <switch.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ldn_data.h"

#define LDN_PORT 62056
#define MAXPKT 1100
#define MAXSENT 160
#define MAXPEND 64
#define CIV7_PASS_LEN 22

static const u8 CIV7_PASS[CIV7_PASS_LEN] = {
    0xcb, 0x96, 0x44, 0x87, 0xb4, 0x49, 0xf4, 0x20, 0x8b, 0x1e, 0xab, 0xee, 0x6e,
    0x92, 0x7f, 0x89, 0x47, 0xfa, 0xd3, 0xd8, 0x8c, 0x37,
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_active;
static bool g_ldn_inited;
static volatile bool g_wifi_off = false;
static volatile bool g_status_run = false;
static char g_status[256] = "idle";
static char g_log[16384];

static void logf_(const char *fmt, ...) {
    pthread_mutex_lock(&g_lock);
    size_t used = strlen(g_log);
    if (used < sizeof(g_log) - 1) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(g_log + used, sizeof(g_log) - used, fmt, args);
        va_end(args);
        size_t len = strlen(g_log);
        if (len > 0 && g_log[len - 1] != '\n' && len + 2 < sizeof(g_log)) {
            g_log[len] = '\n';
            g_log[len + 1] = '\0';
        }
    }
    pthread_mutex_unlock(&g_lock);
    FILE *f = fopen("sdmc:/switch/C7S-Loader/ldn.log", "a");
    if (f) {
        va_list file_args;
        va_start(file_args, fmt);
        vfprintf(f, fmt, file_args);
        va_end(file_args);
        fputc('\n', f);
        fclose(f);
    }
}

static void set_status(const char *fmt, ...) {
    pthread_mutex_lock(&g_lock);
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    pthread_mutex_unlock(&g_lock);
    logf_("%s", g_status);
}

static void *status_thread(void *arg);

static void restore_wifi(void) {
    if (!g_wifi_off) return;
    g_wifi_off = false;
    Result wrc = nifmSetWirelessCommunicationEnabled(true);
    logf_("LDN: re-enable Wi-Fi rc=%08x", (unsigned)wrc);
}

static void start_status_thread(void) {
    if (!g_status_run) return;
    pthread_t sth;
    if (pthread_create(&sth, NULL, status_thread, NULL) == 0) pthread_detach(sth);
}

static u32 ip_u32(const LdnIpv4Address *addr) {
    return addr->addr;
}

/* Civ 7 uses the IPv4 bytes in reverse order inside the game payload. */
static void ip_bytes(u32 ip, u8 out[4]) {
    out[0] = (u8)(ip >> 24);
    out[1] = (u8)(ip >> 16);
    out[2] = (u8)(ip >> 8);
    out[3] = (u8)ip;
}

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void wr32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

typedef struct {
    u64 seq;
    u16 len;
    u8 data[MAXPKT];
} Pkt;

typedef struct {
    u32 local_ip;
    u32 host_ip;
    u64 next_seq;
    u64 expected;
    bool fresh;
    Pkt sent[MAXSENT];
    int sent_n;
    Pkt pending[MAXPEND];
    int pending_n;
    u64 last_r;
    u64 last_v;
} Transport;

typedef struct {
    u64 at;
    int kind;
    int index;
} Follow;

typedef struct {
    Transport t;
    u32 local_ip;
    u32 host_ip;
    Follow follow[32];
    int follow_n;
    int slot;
    bool joined;
    int received;
    int chat_sent;
} Lobby;

static u64 mono_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static void packet_build(const Transport *t, u8 kind, const u8 *body, size_t len, u8 *out, size_t *out_len) {
    ip_bytes(t->host_ip, out);
    out[4] = kind;
    memcpy(out + 5, body, len);
    *out_len = 5 + len;
}

static bool transport_queue(Transport *t, const u8 *payload, size_t len) {
    if (len + 13 > MAXPKT || t->sent_n >= MAXSENT) return false;
    Pkt *p = &t->sent[t->sent_n++];
    p->seq = t->next_seq++;
    p->len = (u16)(13 + len);
    ip_bytes(t->host_ip, p->data);
    p->data[4] = 'F';
    u64 seq = p->seq;
    for (int i = 0; i < 8; i++) p->data[5 + i] = (u8)(seq >> (8 * i));
    memcpy(p->data + 13, payload, len);
    return true;
}

static void transport_init(Transport *t, u32 local_ip, u32 host_ip, const u8 *initial, size_t len) {
    memset(t, 0, sizeof(*t));
    t->local_ip = local_ip;
    t->host_ip = host_ip;
    t->next_seq = 1;
    t->fresh = true;
    t->last_r = mono_ms();
    t->last_v = t->last_r;
    if (len >= 13) t->next_seq = 0;
    /* The initial FireCONN packet is sequence 0. */
    if (len + 5 <= MAXPKT && t->sent_n < MAXSENT) {
        Pkt *p = &t->sent[t->sent_n++];
        p->seq = 0;
        p->len = (u16)(len + 5);
        ip_bytes(host_ip, p->data);
        p->data[4] = 'F';
        memcpy(p->data + 5, initial, len);
        t->next_seq = 1;
    }
}

static bool packet_sent(const Transport *t, u64 seq, u8 *out, size_t *len) {
    for (int i = 0; i < t->sent_n; i++) {
        if (t->sent[i].seq == seq) {
            *len = t->sent[i].len;
            memcpy(out, t->sent[i].data, t->sent[i].len);
            return true;
        }
    }
    return false;
}

static int transport_receive(Transport *t, const u8 *data, size_t len,
                             u8 bodies[][MAXPKT], int *body_lens, int max_bodies,
                             u8 responses[][MAXPKT], int *resp_lens, int max_resp) {
    int nb = 0, nr = 0;
    u8 localb[4];
    ip_bytes(t->local_ip, localb);
    if (len < 5 || memcmp(data, localb, 4) != 0) return 0;
    u8 kind = data[4];
    const u8 *body = data + 5;
    size_t blen = len - 5;

    if (kind == 'S') {
        if (blen >= 1 && body[0] == 'P' && nr < max_resp) {
            u8 tmp[8];
            ip_bytes(t->host_ip, responses[nr]);
            responses[nr][4] = 'S';
            responses[nr][5] = 'p';
            resp_lens[nr++] = 6;
            (void)tmp;
        }
        (void)body_lens;
        return nr;
    }
    if ((kind != 'F' && kind != 'R' && kind != 'V') || blen < 8) return 0;
    u64 seq = 0;
    for (int i = 7; i >= 0; i--) seq = (seq << 8) | body[i];

    if (kind == 'R') {
        size_t plen = 0;
        u8 tmp[MAXPKT];
        if (packet_sent(t, seq, tmp, &plen) && nr < max_resp) {
            memcpy(responses[nr], tmp, plen);
            resp_lens[nr++] = (int)plen;
        }
        (void)body_lens;
        return nr;
    }
    if (kind == 'V') {
        u64 last = t->next_seq - 1;
        if (seq <= last && nr < max_resp) {
            size_t plen = 0;
            u8 tmp[MAXPKT];
            if (packet_sent(t, last, tmp, &plen)) {
                memcpy(responses[nr], tmp, plen);
                resp_lens[nr++] = (int)plen;
            }
        }
        (void)body_lens;
        return nr;
    }

    /* F packet */
    const u8 *payload = body + 8;
    size_t plen = blen - 8;
    if (seq == 0) {
        t->expected = 0;
        t->pending_n = 0;
    } else if (t->fresh) {
        t->expected = seq;
    }
    t->fresh = false;
    if (seq < t->expected) return nr;
    if (seq > t->expected) {
        if (t->pending_n < MAXPEND && plen < MAXPKT) {
            t->pending[t->pending_n].seq = seq;
            t->pending[t->pending_n].len = (u16)plen;
            memcpy(t->pending[t->pending_n].data, payload, plen);
            t->pending_n++;
        }
        return nr;
    }
    if (nb < max_bodies && plen <= MAXPKT) {
        memcpy(bodies[nb], payload, plen);
        body_lens[nb++] = (int)plen;
    }
    t->expected++;
    for (;;) {
        bool found = false;
        for (int i = 0; i < t->pending_n; i++) {
            if (t->pending[i].seq == t->expected) {
                if (nb < max_bodies) {
                    memcpy(bodies[nb], t->pending[i].data, t->pending[i].len);
                    body_lens[nb++] = t->pending[i].len;
                }
                memmove(&t->pending[i], &t->pending[i + 1],
                        (size_t)(t->pending_n - i - 1) * sizeof(Pkt));
                t->pending_n--;
                t->expected++;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    return nr;
}

static size_t text_block(u8 *out, const char *text, bool allow_empty) {
    size_t n = strlen(text);
    if ((!allow_empty && n == 0) || n > 63) n = n > 63 ? 63 : n;
    memset(out, 0, 64);
    if (n) memcpy(out, text, n);
    return 64;
}

static size_t intro_build(u8 *out) {
    size_t n = 0;
    wr32(out + n, 0); n += 4;
    memset(out + n, 0, 64); memcpy(out + n, "Civ7-PC", 7); n += 64;
    memset(out + n, 0, 64); memcpy(out + n, "Civ7-PC", 7); n += 64;
    wr32(out + n, 6); n += 4;
    out[n++] = 0;
    wr32(out + n, 0); n += 4;
    return n;
}

static size_t connection_payload(u8 *out, u32 host_ip) {
    static const u8 CONN[8] = {0x4e, 0x4e, 0x4f, 0x43, 0x65, 0x72, 0x69, 0x46};
    size_t n = 0;
    memset(out, 0, 8); n += 8; /* sequence */
    memcpy(out + n, CONN, 8); n += 8;
    ip_bytes(host_ip, out + n); n += 4;
    memset(out + n, 0, 4); n += 4;
    n += intro_build(out + n);
    wr32(out, 0);
    (void)host_ip;
    return n;
}

static size_t msg_build(u8 *out, u32 kind, u32 counter, const u8 *body, size_t blen) {
    wr32(out, kind);
    wr32(out + 4, 1);
    wr32(out + 8, 1);
    wr32(out + 12, 0xffffffffu);
    wr32(out + 16, 0xffffffffu);
    wr32(out + 20, 1);
    wr32(out + 24, counter);
    if (body && blen) memcpy(out + 28, body, blen);
    return 28 + blen;
}

static size_t net_register(u8 *out, u32 counter) {
    u8 body[4 + 5];
    wr32(body, 5);
    memcpy(body + 4, "1.0.0", 5);
    return msg_build(out, 0x4B8, counter, body, sizeof(body));
}

static size_t net_app_register(u8 *out, u32 counter, int slot) {
    u8 body[5];
    wr32(body, (u32)slot);
    body[4] = 0;
    return msg_build(out, 0x4B9, counter, body, sizeof(body));
}

static size_t net_snapshot(u8 *out, u32 counter) {
    u8 body[5];
    wr32(body, 0);
    body[4] = 1;
    return msg_build(out, 0x4E9, counter, body, sizeof(body));
}

static size_t net_chat(u8 *out, u32 counter, const char *text, int slot) {
    u8 body[8 + 1000 + 8];
    size_t tl = strlen(text);
    if (tl > 1000) tl = 1000;
    wr32(body, (u32)slot);
    wr32(body + 4, (u32)tl);
    memcpy(body + 8, text, tl);
    wr32(body + 8 + tl, (u32)-2);
    wr32(body + 12 + tl, (u32)-1);
    return msg_build(out, 0x3F3, counter, body, 8 + tl + 8);
}

static void lobby_build(const Lobby *l, int kind, int index, u8 *out, size_t *len) {
    switch (kind) {
        case 1: *len = net_register(out, (u32)l->t.next_seq); break;
        case 2: *len = net_app_register(out, (u32)l->t.next_seq, l->slot); break;
        case 3: *len = net_snapshot(out, (u32)l->t.next_seq); break;
        case 4:
            *len = net_chat(out, (u32)l->t.next_seq,
                            ldn_chat_lines[index] ? ldn_chat_lines[index] : "", l->slot);
            break;
        default: *len = 0; break;
    }
}

static int send_raw(int sock, u32 host_ip, const u8 *data, size_t len) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(LDN_PORT);
    dst.sin_addr.s_addr = host_ip;
    return (int)sendto(sock, data, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static bool is_welcome(const u8 *body, size_t len) {
    static const u8 REPLY[8] = {0x59, 0x4c, 0x50, 0x52, 0x65, 0x72, 0x69, 0x46};
    if (len >= 8 && memcmp(body, REPLY, 8) == 0) return true;
    if (len >= 4 && rd32(body) == 0x4B0) return true;
    return false;
}

static bool is_refused(const u8 *body, size_t len) {
    static const u8 REF[8] = {0x53, 0x55, 0x46, 0x52, 0x65, 0x72, 0x69, 0x46};
    static const u8 CLOSE[8] = {0xb7, 0x3a, 0x13, 0xb9, 0x65, 0x72, 0x69, 0x46};
    if (len >= 8 && (memcmp(body, REF, 8) == 0 || memcmp(body, CLOSE, 8) == 0)) return true;
    if (len >= 4 && (rd32(body) == 0x4BA || rd32(body) == 0x3EC)) return true;
    return false;
}

static void schedule_lobby(Lobby *l, const u8 *welcome, size_t wlen) {
    u32 slot = 1;
    if (wlen >= 44) slot = rd32(welcome + 40);
    l->slot = (int)slot;
    if (l->slot < 1) l->slot = 1;
    u64 now = mono_ms();
    l->joined = true;
    l->follow_n = 0;
    l->follow[l->follow_n++] = (Follow){now + 100, 1, 0};
    l->follow[l->follow_n++] = (Follow){now + 200, 2, 0};
    l->follow[l->follow_n++] = (Follow){now + 400, 3, 0};
    u64 chat = now + 500;
    for (size_t i = 0; i < ldn_chat_line_count && l->follow_n < 32; i++) {
        l->follow[l->follow_n++] = (Follow){chat + (u64)i * 10, 4, (int)i};
    }
    set_status("joined lobby, slot %d, sending %u messages", l->slot, (unsigned)ldn_chat_line_count);
}

static bool lobby_run(Lobby *l, int sock, u32 host_ip) {
    u8 body[512];
    size_t body_len = connection_payload(body, host_ip);
    u8 initial[600];
    ip_bytes(host_ip, initial);
    initial[4] = 'F';
    memcpy(initial + 5, body, body_len);
    size_t initial_len = 5 + body_len;

    transport_init(&l->t, l->local_ip, host_ip, body, body_len);
    l->host_ip = host_ip;
    l->follow_n = 0;
    l->joined = false;
    l->received = 0;
    l->chat_sent = 0;

    u64 start = mono_ms();
    u64 next_attempt = start;
    int attempts = 0;
    u8 pkt[MAXPKT];

    while (mono_ms() - start < 45000) {
        u64 now = mono_ms();
        if (!l->joined && attempts < 3 && now >= next_attempt) {
            attempts++;
            memcpy(pkt, initial, initial_len);
            send_raw(sock, host_ip, pkt, initial_len);
            char desc[64];
            snprintf(desc, sizeof(desc), "FireCONN %d/3", attempts);
            set_status("LDN: %s", desc);
            next_attempt = now + 3000;
        }

        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0) {
            u8 data[2048];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(sock, data, sizeof(data), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                u8 bodies[8][MAXPKT];
                int blens[8];
                u8 resps[8][MAXPKT];
                int rlens[8];
                int nr = transport_receive(&l->t, data, (size_t)n, bodies, blens, 8, resps, rlens, 8);
                for (int i = 0; i < nr; i++) send_raw(sock, host_ip, resps[i], (size_t)rlens[i]);
                int nb = 0;
                for (int i = 0; i < 8; i++) if (blens[i] > 0) nb++;
                for (int i = 0; i < 8; i++) {
                    if (blens[i] <= 0) continue;
                    if (is_refused(bodies[i], (size_t)blens[i])) {
                        set_status("join refused/kicked by host");
                        return false;
                    }
                    if (!l->joined && is_welcome(bodies[i], (size_t)blens[i])) {
                        schedule_lobby(l, bodies[i], (size_t)blens[i]);
                    }
                    l->received++;
                }
            }
        }

        if (l->joined) {
            while (l->follow_n > 0 && l->follow[0].at <= mono_ms()) {
                Follow f = l->follow[0];
                memmove(&l->follow[0], &l->follow[1],
                        (size_t)(l->follow_n - 1) * sizeof(Follow));
                l->follow_n--;
                u8 msg[1200];
                size_t mlen = 0;
                lobby_build(l, f.kind, f.index, msg, &mlen);
                if (mlen && transport_queue(&l->t, msg, mlen)) {
                    Pkt *p = &l->t.sent[l->t.sent_n - 1];
                    send_raw(sock, host_ip, p->data, p->len);
                    if (f.kind == 4) {
                        l->chat_sent++;
                        set_status("LDN: %d/%u chat messages", l->chat_sent,
                                   (unsigned)ldn_chat_line_count);
                    }
                }
            }
            if (l->follow_n == 0 && l->chat_sent >= (int)ldn_chat_line_count) {
                set_status("LDN: all %u chat messages sent", (unsigned)ldn_chat_line_count);
                return true;
            }
        }

        if (mono_ms() - l->t.last_v >= 5000) {
            u8 v[13];
            ip_bytes(l->host_ip, v);
            v[4] = 'V';
            u64 seq = l->t.next_seq - 1;
            for (int i = 0; i < 8; i++) v[5 + i] = (u8)(seq >> (8 * i));
            send_raw(sock, host_ip, v, 13);
            l->t.last_v = mono_ms();
        }
    }
    set_status(l->joined ? "LDN: timed out after join" : "LDN: timed out joining lobby");
    return false;
}

static bool ldn_scan_one(LdnNetworkInfo *out) {
    LdnNetworkInfo nets[24];
    LdnScanFilter filter;
    memset(&filter, 0, sizeof(filter));
    s32 total = 0;
    Result rc = ldnScan(0, &filter, nets, 24, &total);
    logf_("scan ch=0 rc=%08x total=%d", (unsigned)rc, (int)total);
    if (R_FAILED(rc)) return false;
    for (s32 i = 0; i < total && i < 24; i++) {
        logf_("  net ch=%d ssid=%.32s nodes=%d link=%d",
              (int)nets[i].common.channel, nets[i].common.ssid.str,
              (int)nets[i].node_count, (int)nets[i].common.link_level);
        if (nets[i].node_count > 0) {
            *out = nets[i];
            return true;
        }
    }
    return false;
}

static void *ldn_worker(void *arg) {
    (void)arg;
    set_status("LDN: scanning for a Civ 7 wireless lobby");
    logf_("LDN route started");
    if (!g_ldn_inited) {
        set_status("LDN: launch in app mode (hold R over a game)");
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    Result orc = ldnOpenStation();
    LdnState ost = 0;
    if (R_SUCCEEDED(orc)) ldnGetState(&ost);
    logf_("LDN: open station rc=%08x state=%d", (unsigned)orc, (int)ost);
    if (R_FAILED(orc)) {
        set_status("LDN: open station failed (%08x)", (unsigned)orc);
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    LdnNetworkInfo info;
    if (!ldn_scan_one(&info)) {
        set_status("LDN: no network found. Open the Civ 7 wireless lobby.");
        ldnCloseStation();
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    u32 host_ip = ip_u32(&info.nodes[0].ip_addr);
    set_status("LDN: connecting to %u.%u.%u.%u",
               (host_ip >> 24) & 255, (host_ip >> 16) & 255,
               (host_ip >> 8) & 255, host_ip & 255);

    LdnSecurityConfig sec;
    memset(&sec, 0, sizeof(sec));
    sec.passphrase_size = CIV7_PASS_LEN;
    memcpy(sec.passphrase, CIV7_PASS, CIV7_PASS_LEN);

    LdnUserConfig user;
    memset(&user, 0, sizeof(user));
    snprintf(user.user_name, sizeof(user.user_name), "CivSwitch-LDN-NX");

    Result rc = ldnConnect(&sec, &user, info.nodes[0].local_communication_version, 0, &info);
    if (R_FAILED(rc)) {
        set_status("LDN: connect failed (%08x)", (unsigned)rc);
        ldnCloseStation();
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    LdnIpv4Address local_addr;
    LdnSubnetMask mask;
    if (R_FAILED(ldnGetIpv4Address(&local_addr, &mask))) {
        set_status("LDN: no local IP");
        ldnDisconnect();
        ldnCloseStation();
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    u32 local_ip = ip_u32(&local_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        set_status("LDN: UDP socket failed (%d)", errno);
        ldnDisconnect();
        ldnCloseStation();
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(LDN_PORT);
    bind_addr.sin_addr.s_addr = local_ip;
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        set_status("LDN: UDP bind failed (%d)", errno);
        close(sock);
        ldnDisconnect();
        ldnCloseStation();
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    Lobby *lobby = calloc(1, sizeof(Lobby));
    bool ok = false;
    if (!lobby) {
        set_status("LDN: out of memory");
    } else {
        lobby->local_ip = local_ip;
        ok = lobby_run(lobby, sock, host_ip);
        free(lobby);
    }

    close(sock);
    ldnDisconnect();
    ldnCloseStation();
    set_status(ok ? "LDN: finished" : "LDN: stopped");
    logf_("LDN route finished: %s", ok ? "ok" : "failed");
    pthread_mutex_lock(&g_lock);
    g_active = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}


static void *status_thread(void *arg) {
    (void)arg;
    while (g_status_run) {
        if (g_wifi_off) {
            logf_("STATUS: Wi-Fi disabled for LDN");
            for (int i = 0; i < 10 && g_status_run; i++) svcSleepThread(100000000ull);
            continue;
        }
        NifmInternetConnectionType t = 0;
        NifmInternetConnectionStatus st = 0;
        u32 str = 0;
        Result r1 = nifmGetInternetConnectionStatus(&t, &str, &st);
        NifmNetworkProfileData prof;
        memset(&prof, 0, sizeof(prof));
        Result r2 = nifmGetCurrentNetworkProfile(&prof);
        logf_("STATUS: wifi_type=%d status=%d strength=%u ssid='%.*s' (rc %08x/%08x)",
              (int)t, (int)st, (unsigned)str,
              (int)prof.wireless_setting_data.ssid_len, prof.wireless_setting_data.ssid,
              (unsigned)r1, (unsigned)r2);
        for (int i = 0; i < 10 && g_status_run; i++) svcSleepThread(100000000ull);
    }
    return NULL;
}

Result ldn_route_init(void) {
    struct in_addr host;
    host.s_addr = (in_addr_t)gethostid();
    if (!host.s_addr || host.s_addr == htonl(INADDR_LOOPBACK)) {
        logf_("LDN: no network profile/IP; skipping ldnInitialize");
        set_status("LDN: connect same Wi-Fi/LAN as Switch 2, then relaunch");
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    AppletType type = appletGetAppletType();
    logf_("LDN: applet type=%d", (int)type);
    if (type != AppletType_Application) {
        logf_("LDN: app mode required; launch with R over a game");
        set_status("LDN: launch in app mode (hold R over a game)");
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    logf_("LDN: calling ldnInitialize at startup");
    Result rc = ldnInitialize(LdnServiceType_User);
    logf_("LDN: startup ldnInitialize rc=%08x", (unsigned)rc);
    g_ldn_inited = R_SUCCEEDED(rc);
    if (g_ldn_inited) {
        LdnState state = 0;
        Result src = ldnGetState(&state);
        logf_("LDN: startup state=%d (rc=%08x)", (int)state, (unsigned)src);
        set_status("LDN: ready");
    } else {
        set_status("LDN: init failed (%08x)", (unsigned)rc);
    }
    return rc;
}

void ldn_route_start(void) {
    pthread_mutex_lock(&g_lock);
    if (g_active) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!g_ldn_inited) {
        pthread_mutex_unlock(&g_lock);
        set_status("LDN: connect same Wi-Fi/LAN as Switch 2, then relaunch");
        return;
    }
    g_active = true;
    g_log[0] = '\0';
    snprintf(g_status, sizeof(g_status), "starting");
    pthread_mutex_unlock(&g_lock);
    g_status_run = true;

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    int rc = pthread_create(&th, &attr, ldn_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        pthread_mutex_lock(&g_lock);
        g_active = false;
        pthread_mutex_unlock(&g_lock);
        set_status("LDN: could not start worker");
        g_status_run = false;
        return;
    }
    pthread_detach(th);
}

bool ldn_route_active(void) {
    pthread_mutex_lock(&g_lock);
    bool a = g_active;
    pthread_mutex_unlock(&g_lock);
    return a;
}

const char *ldn_route_status(void) {
    return g_status;
}

const char *ldn_route_log(void) {
    return g_log;
}

void ldn_route_shutdown(void) {
    if (g_ldn_inited) {
        ldnCloseStation();
        ldnExit();
        g_ldn_inited = false;
    }
}
