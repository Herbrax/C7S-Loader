#include <switch.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui.h"
#include "raster.h"

extern "C" {
#include "app.h"
#include "c7s.h"
#include "httpd.h"
#include "library.h"
#include "settings.h"
#include "update.h"
#include "switch_init.h"
#include "usb_composite.h"
#include "usb_worker.h"
}

extern "C" {
extern const unsigned char font_regular_ttf[];
extern const unsigned long font_regular_ttf_size;
extern const unsigned char font_bold_ttf[];
extern const unsigned long font_bold_ttf_size;
extern const unsigned char font_mono_ttf[];
extern const unsigned long font_mono_ttf_size;
}

namespace platform {
ImFont *font_regular = nullptr;
ImFont *font_bold = nullptr;
ImFont *font_mono = nullptr;
bool g_windowed = false;
float g_density = 1.0f;
}  // namespace platform

namespace {

constexpr float W = 1920.0f;
constexpr float H = 1080.0f;
constexpr float MARGIN = 36.0f;
constexpr float GAP = 24.0f;
constexpr float TOP = 104.0f;
constexpr float BOTTOM = 92.0f;
constexpr float LIST_X = MARGIN;
constexpr float LIST_Y = TOP + 14.0f;
constexpr float LIST_W = 1120.0f;
constexpr float LIST_H = H - BOTTOM - LIST_Y - 14.0f;
constexpr float SIDE_X = LIST_X + LIST_W + GAP;
constexpr float SIDE_W = W - MARGIN - SIDE_X;
constexpr float SIDE_Y = LIST_Y;
constexpr float SIDE_H = LIST_H;
constexpr float ROW_H = 88.0f;
constexpr float ROW_GAP = 12.0f;
constexpr float ROW_TOP = 96.0f;

enum Action {
    ACT_NONE,
    ACT_UP,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_CROSS,
    ACT_CIRCLE,
    ACT_SQUARE,
    ACT_TRIANGLE,
    ACT_L1,
    ACT_R1,
    ACT_OPTIONS,
    ACT_TOUCHPAD,
};

namespace draw {
const ImU32 BG = IM_COL32(11, 18, 32, 255);
const ImU32 PANEL = IM_COL32(18, 26, 43, 255);
const ImU32 PANEL2 = IM_COL32(25, 35, 58, 255);
const ImU32 LINE = IM_COL32(38, 50, 77, 255);
const ImU32 TEXT = IM_COL32(232, 237, 245, 255);
const ImU32 MUTED = IM_COL32(142, 154, 179, 255);
const ImU32 ACCENT = IM_COL32(47, 140, 255, 255);
const ImU32 ACCENT2 = IM_COL32(255, 196, 64, 255);
const ImU32 OK = IM_COL32(60, 207, 145, 255);
const ImU32 ERR = IM_COL32(255, 107, 107, 255);
const ImU32 SEL = IM_COL32(36, 70, 122, 255);
const ImU32 RED = IM_COL32(227, 38, 54, 255);
const ImU32 RED_HOT = IM_COL32(255, 64, 82, 255);
const ImU32 GOLD = IM_COL32(217, 182, 94, 255);
const ImU32 WHITE = IM_COL32(255, 255, 255, 255);

ImDrawList *dl() { return ImGui::GetBackgroundDrawList(); }

ImFont *pick(bool bold) { return bold ? platform::font_bold : platform::font_regular; }

ImVec2 text_size(float size, const char *t, bool bold = false) {
    return pick(bold)->CalcTextSizeA(size, FLT_MAX, 0, t);
}

void text(float x, float y, float size, ImU32 col, const char *t, bool bold = false) {
    dl()->AddText(pick(bold), size, ImVec2(x, y), col, t);
}

void text_clip(float x, float y, float size, ImU32 col, const char *t, float max_w,
               bool bold = false) {
    ImFont *f = pick(bold);
    if (f->CalcTextSizeA(size, FLT_MAX, 0, t).x <= max_w) {
        dl()->AddText(f, size, ImVec2(x, y), col, t);
        return;
    }
    float ell = f->CalcTextSizeA(size, FLT_MAX, 0, "...").x;
    const char *end = t + strlen(t);
    while (end > t && f->CalcTextSizeA(size, FLT_MAX, 0, t, end).x + ell > max_w) {
        end--;
        while (end > t && ((unsigned char)*end & 0xC0) == 0x80) end--;
    }
    dl()->AddText(f, size, ImVec2(x, y), col, t, end);
    float w = f->CalcTextSizeA(size, FLT_MAX, 0, t, end).x;
    dl()->AddText(f, size, ImVec2(x + w, y), col, "...");
}

void rect(float x, float y, float w, float h, ImU32 col, float rounding = 0) {
    dl()->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), col, rounding);
}

void frame(float x, float y, float w, float h, ImU32 col, float rounding = 0, float thick = 2) {
    dl()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col, rounding, ImDrawFlags_None, thick);
}

float button(float x, float y, float s, Action a) {
    ImDrawList *d = dl();
    const float r = s / 2;
    const ImVec2 c(x + r, y + r);
    const char *label = nullptr;
    switch (a) {
        case ACT_CROSS:
        case ACT_CIRCLE:
        case ACT_SQUARE:
        case ACT_TRIANGLE: {
            const char *letter = a == ACT_CROSS ? "A" : a == ACT_CIRCLE ? "B" : a == ACT_SQUARE ? "Y" : "X";
            ImU32 col = a == ACT_CROSS   ? IM_COL32(124, 168, 255, 255)
                        : a == ACT_CIRCLE ? IM_COL32(255, 107, 129, 255)
                        : a == ACT_SQUARE ? IM_COL32(241, 154, 222, 255)
                                          : IM_COL32(63, 224, 181, 255);
            d->AddCircleFilled(c, r, IM_COL32(32, 44, 70, 255), 32);
            d->AddCircle(c, r - 1.5f, col, 32, 2.0f);
            float fs = s * 0.58f;
            ImVec2 ts = text_size(fs, letter, true);
            d->AddText(platform::font_bold, fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), col, letter);
            return s;
        }
        case ACT_OPTIONS:
        case ACT_TOUCHPAD: {
            const float bar = s * 0.17f, arm = s * 0.27f;
            const ImU32 face = IM_COL32(206, 216, 232, 255), ink = IM_COL32(16, 24, 40, 255);
            d->AddCircleFilled(c, r, face, 32);
            d->AddRectFilled(ImVec2(c.x - arm, c.y - bar / 2), ImVec2(c.x + arm, c.y + bar / 2), ink,
                             bar * 0.35f);
            if (a == ACT_OPTIONS) {
                d->AddRectFilled(ImVec2(c.x - bar / 2, c.y - arm), ImVec2(c.x + bar / 2, c.y + arm), ink,
                                 bar * 0.35f);
            }
            return s;
        }
        case ACT_L1: label = "L"; break;
        case ACT_R1: label = "R"; break;
        case ACT_UP:
        case ACT_DOWN:
        case ACT_LEFT:
        case ACT_RIGHT: {
            float t = s * 0.28f;
            ImU32 dim = IM_COL32(60, 74, 104, 255);
            d->AddRectFilled(ImVec2(c.x - t / 2, y + 2), ImVec2(c.x + t / 2, y + s - 2), dim, 3);
            d->AddRectFilled(ImVec2(x + 2, c.y - t / 2), ImVec2(x + s - 2, c.y + t / 2), dim, 3);
            ImU32 on = TEXT;
            if (a == ACT_UP || a == ACT_DOWN) {
                d->AddRectFilled(ImVec2(c.x - t / 2, y + 2), ImVec2(c.x + t / 2, y + s - 2), on, 3);
            } else {
                d->AddRectFilled(ImVec2(x + 2, c.y - t / 2), ImVec2(x + s - 2, c.y + t / 2), on, 3);
            }
            return s;
        }
        default: return 0;
    }
    float fs = s * 0.52f;
    ImVec2 ts = text_size(fs, label, true);
    float w = ts.x + s * 0.7f;
    if (w < s * 0.8f) w = s * 0.8f;
    d->AddRectFilled(ImVec2(x, y + s * 0.10f), ImVec2(x + w, y + s * 0.90f),
                     IM_COL32(206, 216, 232, 255), s * 0.32f);
    d->AddText(platform::font_bold, fs, ImVec2(x + (w - ts.x) / 2, y + (s - ts.y) / 2),
               IM_COL32(16, 24, 40, 255), label);
    return w;
}

float hint(float x, float y, Action a, const char *caption) {
    const float s = 44;
    float w = button(x, y, s, a);
    if (w <= 0) return 0;
    text(x + w + 12, y + (s - 30) / 2 - 1, 30, MUTED, caption);
    return w + 12 + text_size(30, caption).x + 34;
}


float wordmark(float x, float y, float size) {
    ImDrawList *d = dl();
    const char *a = "C7S";
    const char *b = "-Loader";
    float cx = x;
    for (const char *p = a; *p; p++) {
        char c[2] = {*p, 0};
        d->AddText(platform::font_mono, size, ImVec2(cx, y), IM_COL32(245, 243, 236, 255), c);
        cx += platform::font_mono->CalcTextSizeA(size, FLT_MAX, 0, c).x - 0.02f * size;
    }
    for (const char *p = b; *p; p++) {
        char c[2] = {*p, 0};
        d->AddText(platform::font_mono, size, ImVec2(cx, y), GOLD, c);
        cx += platform::font_mono->CalcTextSizeA(size, FLT_MAX, 0, c).x - 0.02f * size;
    }
    return cx - x;
}

void panel(float x, float y, float w, float h) {
    rect(x, y, w, h, PANEL, 16);
    frame(x, y, w, h, LINE, 16, 1.5f);
}

float pill(float x, float y, const char *label, ImU32 dot) {
    ImVec2 ts = text_size(22, label, true);
    const float w = ts.x + 50, h = 40;
    rect(x, y, w, h, PANEL2, 20);
    frame(x, y, w, h, LINE, 20, 1.0f);
    dl()->AddCircleFilled(ImVec2(x + 20, y + h / 2), 6, dot, 12);
    text(x + 34, y + (h - ts.y) / 2 - 1, 22, TEXT, label, true);
    return w;
}

void red_button(float x, float y, float w, float h, const char *label, bool focused) {
    ImDrawList *d = dl();
    if (focused) d->AddRectFilled(ImVec2(x - 6, y - 6), ImVec2(x + w + 6, y + h + 6),
                                  IM_COL32(255, 255, 255, 26), h * 0.18f);
    d->AddRectFilled(ImVec2(x, y + 6), ImVec2(x + w, y + h + 6), IM_COL32(0, 0, 0, 90), h * 0.16f);
    d->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), RED, h * 0.16f);
    if (focused) d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(255, 255, 255, 210),
                            h * 0.16f, ImDrawFlags_None, 3.0f);
    ImVec2 ts = text_size(48, label, true);
    d->AddText(platform::font_bold, 48, ImVec2(x + (w - ts.x) / 2, y + (h - ts.y) / 2 - 2), WHITE,
               label);
}

void accent_button(float x, float y, float w, float h, const char *label, bool focused) {
    rect(x, y, w, h, focused ? ACCENT : PANEL2, 12);
    frame(x, y, w, h, focused ? WHITE : LINE, 12, focused ? 2.5f : 1.5f);
    ImVec2 ts = text_size(30, label, true);
    text(x + (w - ts.x) / 2, y + (h - ts.y) / 2, 30, focused ? WHITE : TEXT, label, true);
}

void file_icon(float x, float y, float s, ImU32 col) {
    ImDrawList *d = dl();
    float w = s * 0.72f;
    float fold = s * 0.24f;
    float x0 = x + (s - w) / 2;
    d->AddRectFilled(ImVec2(x0, y + s * 0.08f), ImVec2(x0 + w - fold, y + s * 0.92f), col, 2);
    d->AddRectFilled(ImVec2(x0, y + s * 0.08f + fold), ImVec2(x0 + w, y + s * 0.92f), col, 2);
    d->AddTriangleFilled(ImVec2(x0 + w - fold, y + s * 0.08f), ImVec2(x0 + w, y + s * 0.08f + fold),
                         ImVec2(x0 + w - fold, y + s * 0.08f + fold), IM_COL32(200, 210, 228, 255));
}

void spinner(float cx, float cy, float r, ImU32 col) {
    float t = (float)ImGui::GetTime() * 6.0f;
    ImDrawList *d = dl();
    d->PathClear();
    d->PathArcTo(ImVec2(cx, cy), r, t, t + 4.2f, 24);
    d->PathStroke(col, ImDrawFlags_None, 4.0f);
}
}  // namespace draw

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool hit(float tx, float ty) const { return w > 0 && h > 0 && tx >= x && tx <= x + w && ty >= y && ty <= y + h; }
};

struct Input {
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, x = false, y = false, plus = false, minus = false;
    bool touch = false;
    float tx = 0, ty = 0;
};

struct Hint {
    Action a;
    const char *text;
};

enum Screen { SCREEN_HOME, SCREEN_CONFIRM, SCREEN_RUNNING, SCREEN_RESULT, SCREEN_SETTINGS };

NWindow *g_win = nullptr;
Framebuffer g_fb;
bool g_fb_ready = false;
PadState g_pad;
bool g_touch_ready = false;
uint64_t g_last_frame = 0;
uint64_t g_prev_buttons = 0;
bool g_prev_touch = false;

Screen g_screen = SCREEN_HOME;
int g_confirm_focus = 0;
C7sEntry g_entries[64];
int g_count = 0;
int g_cursor = 0;
int g_first = 0;
uint64_t g_last_refresh = 0;
bool g_quit = false;
char g_toast[192] = {0};
uint64_t g_toast_until = 0;

Rect g_row_rects[64];
Rect g_start_rect;
Rect g_refresh_rect;
Rect g_port_rect;
Rect g_cancel_rect;
Rect g_change_rect;
Rect g_back_rect;

uint64_t ticks_ms() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

void set_toast(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, args);
    va_end(args);
    g_toast_until = ticks_ms() + 3500;
}

bool gfx_init() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    g_touch_ready = true;

    g_win = nwindowGetDefault();
    if (!g_win) return false;
    if (R_FAILED(framebufferCreate(&g_fb, g_win, (u32)W, (u32)H, PIXEL_FORMAT_RGBA_8888, 2))) {
        return false;
    }
    if (R_FAILED(framebufferMakeLinear(&g_fb))) {
        framebufferClose(&g_fb);
        return false;
    }
    g_fb_ready = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(W, H);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendRendererName = "c7s-raster";

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = cfg.OversampleV = 2;
    platform::font_regular = io.Fonts->AddFontFromMemoryTTF(
        (void *)font_regular_ttf, (int)font_regular_ttf_size, 40.0f, &cfg);
    platform::font_bold = io.Fonts->AddFontFromMemoryTTF(
        (void *)font_bold_ttf, (int)font_bold_ttf_size, 52.0f, &cfg);
    platform::font_mono = io.Fonts->AddFontFromMemoryTTF(
        (void *)font_mono_ttf, (int)font_mono_ttf_size, 52.0f, &cfg);
    g_last_frame = ticks_ms();
    return platform::font_regular && platform::font_bold && platform::font_mono;
}

void gfx_shutdown() {
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (g_fb_ready) {
        framebufferClose(&g_fb);
        g_fb_ready = false;
    }
    g_win = nullptr;
}

void frame_begin() {
    ImGuiIO &io = ImGui::GetIO();
    uint64_t now = ticks_ms();
    io.DeltaTime = now > g_last_frame ? (float)(now - g_last_frame) / 1000.0f : 1.0f / 60.0f;
    g_last_frame = now;
    io.DisplaySize = ImVec2(W, H);
    ImGui::NewFrame();
}

void frame_end() {
    ImGui::Render();
    ImDrawData *data = ImGui::GetDrawData();
    raster::update_textures(data);
    u32 stride = 0;
    uint32_t *dst = (uint32_t *)framebufferBegin(&g_fb, &stride);
    if (!dst) return;
    raster::render(data, raster::Target{dst, (int)W, (int)H, (int)(stride / 4)},
                   IM_COL32(11, 18, 32, 255));
    framebufferEnd(&g_fb);
}

Input poll_input() {
    Input in;
    if (!appletMainLoop()) {
        g_quit = true;
        return in;
    }
    padUpdate(&g_pad);
    uint64_t cur = padGetButtons(&g_pad);
    uint64_t down = cur & ~g_prev_buttons;
    g_prev_buttons = cur;

    in.up = (down & HidNpadButton_AnyUp) != 0;
    in.down = (down & HidNpadButton_AnyDown) != 0;
    in.left = (down & HidNpadButton_AnyLeft) != 0;
    in.right = (down & HidNpadButton_AnyRight) != 0;
    in.a = (down & HidNpadButton_A) != 0;
    in.b = (down & HidNpadButton_B) != 0;
    in.x = (down & HidNpadButton_X) != 0;
    in.y = (down & HidNpadButton_Y) != 0;
    in.plus = (down & HidNpadButton_Plus) != 0;
    in.minus = (down & HidNpadButton_Minus) != 0;

    HidTouchScreenState ts{};
    if (g_touch_ready && hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        in.touch = !g_prev_touch;
        in.tx = (float)ts.touches[0].x * (W / 1280.0f);
        in.ty = (float)ts.touches[0].y * (H / 720.0f);
        g_prev_touch = true;
    } else {
        g_prev_touch = false;
    }
    return in;
}

void refresh_entries(bool prefer_selected) {
    char keep[128] = {0};
    if (g_count > 0 && g_cursor >= 0 && g_cursor < g_count) {
        snprintf(keep, sizeof(keep), "%s", g_entries[g_cursor].name);
    }
    C7sEntry fresh[64];
    int n = c7s_list(fresh, 64);
    memcpy(g_entries, fresh, sizeof(C7sEntry) * (size_t)n);
    g_count = n;

    char wanted[128] = {0};
    if (prefer_selected) {
        char app_sel[128];
        app_get_selected(app_sel, sizeof(app_sel));
        if (app_sel[0]) snprintf(wanted, sizeof(wanted), "%s", app_sel);
        else if (keep[0]) snprintf(wanted, sizeof(wanted), "%s", keep);
    } else if (keep[0]) {
        snprintf(wanted, sizeof(wanted), "%s", keep);
    }

    if (g_count == 0) {
        g_cursor = 0;
        g_first = 0;
    } else {
        if (wanted[0]) {
            for (int i = 0; i < g_count; i++) {
                if (strcmp(g_entries[i].name, wanted) == 0) {
                    g_cursor = i;
                    break;
                }
            }
        }
        if (g_cursor >= g_count) g_cursor = g_count - 1;
        if (g_cursor < 0) g_cursor = 0;
    }
    g_last_refresh = ticks_ms();
}

void draw_top(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw::wordmark(MARGIN, 18, 48);

    char web[160];
    snprintf(web, sizeof(web), "http://%s:%u/", s.ip[0] && strcmp(s.ip, "unknown") ? s.ip : "console",
             (unsigned)s.port);
    bool usb_ok = u && u->initialized && u->pad_ready && u->kbd_ready;
    const char *usb_txt = (!u || !u->initialized) ? "USB FAILED" : usb_ok ? "USB READY" : "USB WAITING";
    ImU32 dot = (!u || !u->initialized) ? draw::ERR : usb_ok ? draw::OK : draw::ACCENT2;
    bool web_ok = httpd_running();
    if (!web_ok) snprintf(web, sizeof(web), "web server offline");

    float ww = draw::text_size(22, web, true).x + 50;
    float uw = draw::text_size(22, usb_txt, true).x + 50;
    float wx = W - MARGIN - ww;
    float ux = wx - 12 - uw;
    draw::pill(ux, 30, usb_txt, dot);
    draw::pill(wx, 30, web, web_ok ? draw::ACCENT : draw::ERR);
}

void draw_bottom(const Hint *hints, int n) {
    draw::rect(0, H - BOTTOM, W, BOTTOM, draw::PANEL, 0);
    draw::rect(0, H - BOTTOM, W, 1.5f, draw::LINE, 0);
    float x = MARGIN;
    float y = H - BOTTOM + (BOTTOM - 44) / 2 - 2;
    for (int i = 0; i < n; i++) x += draw::hint(x, y, hints[i].a, hints[i].text);
}

void draw_toast() {
    if (!g_toast[0] || ticks_ms() >= g_toast_until) return;
    ImVec2 ts = draw::text_size(28, g_toast, true);
    float w = ts.x + 56, h = 64;
    float x = (W - w) / 2, y = TOP + 20;
    draw::rect(x, y, w, h, draw::PANEL2, 14);
    draw::frame(x, y, w, h, draw::ACCENT, 14, 2);
    draw::text(x + 28, y + (h - ts.y) / 2, 28, draw::TEXT, g_toast, true);
}

void format_size(long long n, char *buf, size_t len) {
    if (n < 1024) snprintf(buf, len, "%lld B", n);
    else if (n < 1024 * 1024) snprintf(buf, len, "%.1f KB", (double)n / 1024.0);
    else snprintf(buf, len, "%.1f MB", (double)n / (1024.0 * 1024.0));
}

void draw_home(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_top(s, u);

    draw::panel(LIST_X, LIST_Y, LIST_W, LIST_H);
    draw::text(LIST_X + 24, LIST_Y + 18, 28, draw::MUTED, "PAYLOADS", true);
    draw::text(LIST_X + 24, LIST_Y + 56, 22, draw::MUTED, "X refresh   Y port   + exit");

    g_refresh_rect = {LIST_X + LIST_W - 176, LIST_Y + 14, 152, 46};
    draw::rect(g_refresh_rect.x, g_refresh_rect.y, g_refresh_rect.w, g_refresh_rect.h, draw::PANEL2, 10);
    draw::frame(g_refresh_rect.x, g_refresh_rect.y, g_refresh_rect.w, g_refresh_rect.h, draw::LINE, 10,
                1.5f);
    ImVec2 rts = draw::text_size(24, "Refresh", true);
    draw::text(g_refresh_rect.x + (g_refresh_rect.w - rts.x) / 2,
               g_refresh_rect.y + (g_refresh_rect.h - rts.y) / 2, 24, draw::TEXT, "Refresh", true);

    int visible = (int)((LIST_H - ROW_TOP - 16) / (ROW_H + ROW_GAP));
    if (visible < 1) visible = 1;
    if (g_cursor < g_first) g_first = g_cursor;
    if (g_cursor >= g_first + visible) g_first = g_cursor - visible + 1;
    if (g_first < 0) g_first = 0;

    for (int i = 0; i < 64; i++) g_row_rects[i] = Rect{};
    for (int i = 0; i < visible; i++) {
        int idx = g_first + i;
        if (idx >= g_count) break;
        C7sEntry &e = g_entries[idx];
        Rect r{LIST_X + 16, LIST_Y + ROW_TOP + i * (ROW_H + ROW_GAP), LIST_W - 32, ROW_H};
        g_row_rects[idx] = r;
        bool sel = idx == g_cursor;
        draw::rect(r.x, r.y, r.w, r.h, sel ? draw::SEL : draw::PANEL2, 12);
        draw::frame(r.x, r.y, r.w, r.h, sel ? draw::ACCENT : draw::LINE, 12, sel ? 2.0f : 1.2f);
        if (sel) draw::rect(r.x, r.y + 10, 4, r.h - 20, draw::ACCENT, 2);

        draw::file_icon(r.x + 18, r.y + 20, 46, sel ? draw::TEXT : draw::MUTED);
        float tx = r.x + 80;
        draw::text_clip(tx, r.y + 12, 32, draw::TEXT, e.name, r.w - 80 - 140, true);
        char meta[220];
        if (e.header) {
            snprintf(meta, sizeof(meta), "%s - %d lines%s", e.title, e.declared,
                     e.mismatch ? " (count differs)" : "");
        } else {
            snprintf(meta, sizeof(meta), "header not recognised - %d lines", e.lines);
        }
        draw::text_clip(tx, r.y + 54, 22, e.mismatch ? draw::ERR : draw::MUTED, meta, r.w - 80 - 140);

        char sz[32];
        format_size(e.size, sz, sizeof(sz));
        ImVec2 sts = draw::text_size(22, sz);
        draw::text(r.x + r.w - 18 - sts.x, r.y + 32, 22, draw::MUTED, sz);
    }

    if (g_count == 0) {
        const char *a = "No .C7S files";
        const char *b = "Open the web app and upload one.";
        ImVec2 ta = draw::text_size(34, a, true);
        ImVec2 tb = draw::text_size(28, b);
        float cx = LIST_X + LIST_W / 2;
        float cy = LIST_Y + LIST_H / 2 - 30;
        draw::text(cx - ta.x / 2, cy, 34, draw::TEXT, a, true);
        draw::text(cx - tb.x / 2, cy + 54, 28, draw::MUTED, b);
    }

    if (g_first > 0) {
        ImVec2 t = draw::text_size(22, "^");
        draw::text(LIST_X + LIST_W / 2 - t.x / 2, LIST_Y + ROW_TOP - 22, 22, draw::MUTED, "^");
    }
    if (g_first + visible < g_count) {
        ImVec2 t = draw::text_size(22, "v");
        draw::text(LIST_X + LIST_W / 2 - t.x / 2, LIST_Y + LIST_H - 26, 22, draw::MUTED, "v");
    }

    draw::panel(SIDE_X, SIDE_Y, SIDE_W, SIDE_H);
    draw::text(SIDE_X + 24, SIDE_Y + 18, 28, draw::MUTED, "SELECTED", true);

    if (g_count > 0) {
        C7sEntry &e = g_entries[g_cursor];
        float x = SIDE_X + 24;
        float y = SIDE_Y + 58;
        draw::text_clip(x, y, 34, draw::TEXT, e.name, SIDE_W - 48, true);
        y += 46;
        if (e.header) {
            draw::text_clip(x, y, 30, draw::TEXT, e.title, SIDE_W - 48);
            y += 34;
            char line[96];
            snprintf(line, sizeof(line), "%d lines (header says %d)", e.lines, e.declared);
            draw::text(x, y, 24, e.mismatch ? draw::ERR : draw::MUTED, line);
            y += 28;
            if (e.mismatch) {
                draw::text(x, y, 24, draw::ERR, "The file's line count differs.", true);
                y += 26;
            }
        } else {
            draw::text(x, y, 30, draw::MUTED, "header not recognised");
            y += 34;
            char line[64];
            snprintf(line, sizeof(line), "%d lines", e.lines);
            draw::text(x, y, 24, draw::MUTED, line);
            y += 30;
        }

        draw::rect(x, y + 10, SIDE_W - 48, 1.5f, draw::LINE, 0);
        y += 34;
        draw::text(x, y, 26, draw::MUTED, "WEB APP", true);
        y += 30;
        char url[160];
        snprintf(url, sizeof(url), "http://%s:%u/",
                 s.ip[0] && strcmp(s.ip, "unknown") ? s.ip : "console", (unsigned)s.port);
        draw::text_clip(x, y, 28, draw::ACCENT, url, SIDE_W - 48);
        y += 32;
        draw::text(x, y, 22, draw::MUTED, "Upload from your PC or phone.");
        y += 34;

        g_port_rect = {x, y, 210, 46};
        draw::rect(g_port_rect.x, g_port_rect.y, g_port_rect.w, g_port_rect.h, draw::PANEL2, 10);
        draw::frame(g_port_rect.x, g_port_rect.y, g_port_rect.w, g_port_rect.h, draw::LINE, 10, 1.5f);
        ImVec2 pts = draw::text_size(24, "Change port", true);
        draw::text(g_port_rect.x + (g_port_rect.w - pts.x) / 2,
                   g_port_rect.y + (g_port_rect.h - pts.y) / 2, 24, draw::TEXT, "Change port", true);
        y += 62;

        bool usb_ok = u && u->initialized && u->pad_ready && u->kbd_ready;
        if (!usb_ok) {
            draw::rect(x, y, SIDE_W - 48, 100, draw::PANEL2, 12);
            draw::frame(x, y, SIDE_W - 48, 100, draw::ERR, 12, 1.5f);
            draw::text(x + 16, y + 14, 24, draw::ERR,
                       (!u || !u->initialized) ? "USB did not start" : "USB not ready yet", true);
            char detail[160];
            if (!u || !u->initialized) {
                snprintf(detail, sizeof(detail), "%s", (u && u->stage[0]) ? u->stage : "check usb.log");
            } else {
                snprintf(detail, sizeof(detail), "gamepad %s - keyboard %s",
                         u->pad_ready ? "ready" : "waiting", u->kbd_ready ? "ready" : "waiting");
            }
            draw::text_clip(x + 16, y + 52, 22, draw::MUTED, detail, SIDE_W - 80);
        }

        float bw = SIDE_W - 48, bh = 108;
        float bx = SIDE_X + 24, by = SIDE_Y + SIDE_H - 24 - bh;
        draw::red_button(bx, by, bw, bh, "START", true);
        g_start_rect = {bx, by, bw, bh};
    } else {
        g_start_rect = Rect{};
        g_port_rect = Rect{};
    }

    Hint hints[] = {{ACT_CROSS, "Start"}, {ACT_TRIANGLE, "Refresh"}, {ACT_SQUARE, "Port"},
                    {ACT_TOUCHPAD, "Test keys"}, {ACT_OPTIONS, "Exit"}};
    draw_bottom(hints, 5);
}

void draw_confirm(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_home(s, u);
    draw::dl()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(4, 8, 16, 190));

    const float dw = 1000, dh = 580;
    const float dx = (W - dw) / 2, dy = (H - dh) / 2;
    draw::panel(dx, dy, dw, dh);
    draw::text(dx + 40, dy + 30, 50, draw::TEXT, "Are you sure?", true);

    if (g_count > 0) {
        C7sEntry &e = g_entries[g_cursor];
        char line[260];
        snprintf(line, sizeof(line), "Load %s?", e.name);
        draw::text_clip(dx + 40, dy + 108, 34, draw::TEXT, line, dw - 80);
        if (e.header) {
            snprintf(line, sizeof(line), "Payload %s - lines %d.", e.title, e.declared);
            draw::text_clip(dx + 40, dy + 154, 30, draw::MUTED, line, dw - 80);
            if (e.mismatch) {
                snprintf(line, sizeof(line), "Warning: the file has %d payload lines.", e.lines);
                draw::text(dx + 40, dy + 194, 26, draw::ERR, line, true);
            }
        } else {
            snprintf(line, sizeof(line), "Payload header not recognised (%d lines).", e.lines);
            draw::text_clip(dx + 40, dy + 148, 26, draw::MUTED, line, dw - 80);
        }
    }
    draw::text(dx + 40, dy + 240, 26, draw::MUTED,
               "This will type the payload into the other console over USB.");

    bool usb_ok = u && u->initialized && u->pad_ready && u->kbd_ready;
    if (!usb_ok) {
        draw::text(dx + 40, dy + 280, 26, draw::ACCENT2,
                   "USB is not ready. START will fail until it is.", true);
    }

    g_cancel_rect = {dx + 40, dy + dh - 156, 240, 72};
    draw::accent_button(g_cancel_rect.x, g_cancel_rect.y, g_cancel_rect.w, g_cancel_rect.h, "Cancel",
                        g_confirm_focus == 1);

    float bw = 440, bh = 104;
    float bx = dx + (dw - bw) / 2 + 30, by = dy + dh - 172;
    draw::red_button(bx, by, bw, bh, "START", g_confirm_focus == 0);
    g_start_rect = {bx, by, bw, bh};

    Hint hints[] = {{ACT_CROSS, "Start"}, {ACT_CIRCLE, "Cancel"}};
    draw_bottom(hints, 2);
}

void draw_running(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_top(s, u);

    const float cw = 1060, ch = 540;
    const float cx = (W - cw) / 2, cy = (H - ch) / 2 - 10;
    draw::panel(cx, cy, cw, ch);
    draw::text(cx + 32, cy + 26, 28, draw::MUTED, "RUNNING", true);
    draw::text_clip(cx + 32, cy + 64, 38, draw::TEXT,
                    s.selected[0] ? s.selected : "payload", cw - 64, true);

    const float bx = cx + 32, by = cy + 150, bw = cw - 64, bh = 22;
    draw::rect(bx, by, bw, bh, draw::PANEL2, bh / 2);
    float pct = s.total > 0 ? (float)s.step / (float)s.total : 0;
    if (pct > 1) pct = 1;
    if (pct > 0) draw::rect(bx, by, bw * pct, bh, draw::ACCENT, bh / 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "%d%%", (int)(pct * 100));
    draw::text(bx, by - 36, 26, draw::TEXT, buf, true);
    snprintf(buf, sizeof(buf), "step %d / %d", s.step, s.total);
    draw::text(bx, by + 44, 28, draw::MUTED, buf);

    if (s.running) draw::spinner(cx + cw - 76, cy + 76, 20, draw::ACCENT);
    else if (!s.finished) draw::text(cx + 32, cy + 220, 30, draw::MUTED, "Starting...");

    draw::text(cx + 32, cy + 260, 24, draw::MUTED,
               "Keep the USB cable connected. Do not touch the consoles.");

    const float bw2 = 340, bh2 = 84;
    const float bx2 = cx + (cw - bw2) / 2, by2 = cy + ch - 140;
    draw::red_button(bx2, by2, bw2, bh2, "CANCEL", true);
    g_cancel_rect = {bx2, by2, bw2, bh2};

    Hint hints[] = {{ACT_CIRCLE, "Cancel"}, {ACT_OPTIONS, "Exit"}};
    draw_bottom(hints, 2);
}

void draw_result(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_top(s, u);
    const float cw = 960, ch = 460;
    const float cx = (W - cw) / 2, cy = (H - ch) / 2 - 10;
    draw::panel(cx, cy, cw, ch);
    draw::text(cx + 32, cy + 26, 28, draw::MUTED, "RESULT", true);
    draw::text(cx + 32, cy + 66, 50, s.ok ? draw::OK : draw::ERR, s.ok ? "Finished" : "Stopped", true);
    draw::text_clip(cx + 32, cy + 140, 30, draw::MUTED, s.message, cw - 64);
    draw::text_clip(cx + 32, cy + 186, 26, draw::MUTED, s.selected, cw - 64);

    const float bw = 280, bh = 78;
    const float bx = cx + (cw - bw) / 2, by = cy + ch - 110;
    draw::accent_button(bx, by, bw, bh, "BACK", true);
    g_back_rect = {bx, by, bw, bh};

    Hint hints[] = {{ACT_CROSS, "Back"}, {ACT_CIRCLE, "Back"}};
    draw_bottom(hints, 2);
}

bool ask_port(unsigned short current, unsigned short *out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&kbd, "Web app port");
    swkbdConfigSetGuideText(&kbd, "1 to 65535");
    char initial[16];
    snprintf(initial, sizeof(initial), "%u", (unsigned)current);
    swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, 5);
    swkbdConfigSetInitialCursorPos(&kbd, 1);
    char buf[16] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    padUpdate(&g_pad);
    g_prev_buttons = padGetButtons(&g_pad);
    if (R_FAILED(rc)) return false;
    int p = atoi(buf);
    if (p < 1 || p > 65535) return false;
    *out = (unsigned short)p;
    return true;
}

void draw_settings(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_top(s, u);
    const float cw = 900, ch = 500;
    const float cx = (W - cw) / 2, cy = (H - ch) / 2 - 10;
    draw::panel(cx, cy, cw, ch);
    draw::text(cx + 32, cy + 26, 28, draw::MUTED, "SETTINGS", true);
    draw::text(cx + 32, cy + 72, 36, draw::TEXT, "Web app port", true);
    char port[16];
    snprintf(port, sizeof(port), "%u", (unsigned)s.port);
    draw::text(cx + 32, cy + 122, 88, draw::ACCENT2, port, true);
    draw::text(cx + 32, cy + 236, 24, draw::MUTED, "Changing the port restarts the web server.");

    g_change_rect = {cx + 32, cy + ch - 180, 280, 72};
    draw::accent_button(g_change_rect.x, g_change_rect.y, g_change_rect.w, g_change_rect.h,
                        "Change port", true);
    g_back_rect = {cx + cw - 232, cy + ch - 180, 200, 72};
    draw::accent_button(g_back_rect.x, g_back_rect.y, g_back_rect.w, g_back_rect.h, "Back", false);

    Hint hints[] = {{ACT_CROSS, "Change"}, {ACT_CIRCLE, "Back"}};
    draw_bottom(hints, 2);
}

void start_selected() {
    const UsbCompositeStatus *u = usb_composite_status();
    if (!u->initialized) {
        set_toast("USB did not initialize: %s", u->stage[0] ? u->stage : "check usb.log");
        return;
    }
    if (g_count <= 0) return;
    app_request_start(g_entries[g_cursor].name);
    set_toast("Starting in 5s - focus the target text field");
    g_screen = SCREEN_RUNNING;
}

void handle_input(const Input &in, const C7sAppSnapshot &s) {
    switch (g_screen) {
        case SCREEN_HOME:
            if (g_count > 0) {
                if (in.up) g_cursor = (g_cursor + g_count - 1) % g_count;
                if (in.down) g_cursor = (g_cursor + 1) % g_count;
            }
            if (in.a && g_count > 0) { g_confirm_focus = 0; g_screen = SCREEN_CONFIRM; }
            if (in.x) refresh_entries(false);
            if (in.y) g_screen = SCREEN_SETTINGS;
            if (in.minus) {
                app_request_ktest();
                set_toast("Test keys in 5s - focus a text field");
            }
            if (in.plus) g_quit = true;
            if (in.touch) {
                for (int i = 0; i < g_count; i++) {
                    if (g_row_rects[i].hit(in.tx, in.ty)) g_cursor = i;
                }
                if (g_start_rect.hit(in.tx, in.ty) && g_count > 0) { g_confirm_focus = 0; g_screen = SCREEN_CONFIRM; }
                if (g_refresh_rect.hit(in.tx, in.ty)) refresh_entries(false);
                if (g_port_rect.hit(in.tx, in.ty)) g_screen = SCREEN_SETTINGS;
            }
            break;
        case SCREEN_CONFIRM:
            if (in.left || in.right || in.up || in.down) g_confirm_focus = 1 - g_confirm_focus;
            if (in.a) {
                if (g_confirm_focus == 0) start_selected();
                else g_screen = SCREEN_HOME;
            }
            if (in.b) g_screen = SCREEN_HOME;
            if (in.touch) {
                if (g_start_rect.hit(in.tx, in.ty)) { g_confirm_focus = 0; start_selected(); }
                if (g_cancel_rect.hit(in.tx, in.ty)) g_screen = SCREEN_HOME;
            }
            break;
        case SCREEN_RUNNING:
            if (in.b) app_request_cancel();
            if (in.plus) {
                app_request_cancel();
                g_quit = true;
            }
            if (in.touch && g_cancel_rect.hit(in.tx, in.ty)) app_request_cancel();
            break;
        case SCREEN_RESULT:
            if (in.a || in.b || (in.touch && g_back_rect.hit(in.tx, in.ty))) {
                g_screen = SCREEN_HOME;
                refresh_entries(false);
            }
            break;
        case SCREEN_SETTINGS:
            if (in.a || (in.touch && g_change_rect.hit(in.tx, in.ty))) {
                unsigned short np;
                if (ask_port(s.port, &np)) {
                    settings_save_port(np);
                    app_request_restart(np);
                    set_toast("Restarting web server on port %u...", (unsigned)np);
                    g_screen = SCREEN_HOME;
                }
            }
            if (in.b || (in.touch && g_back_rect.hit(in.tx, in.ty))) g_screen = SCREEN_HOME;
            break;
    }
}

}  // namespace

extern "C" void chiral_imgui_check_failed(const char *expr, const char *file, int line) {
    fprintf(stderr, "ImGui check failed: %s (%s:%d)\n", expr, file, line);
}

extern "C" int c7s_ui_run(void) {
    if (!gfx_init()) {
        gfx_shutdown();
        return 1;
    }
    refresh_entries(true);

    while (!g_quit) {
        unsigned short new_port;
        if (app_take_restart(&new_port)) {
            httpd_stop();
            httpd_start(new_port);
            set_toast("Web server on port %u", (unsigned)new_port);
        }

        if (app_take_update()) {
            frame_begin();
            draw::rect(0, 0, W, H, draw::BG, 0);
            ImVec2 ts = draw::text_size(42, "Applying update...", true);
            draw::text((W - ts.x) / 2, (H - ts.y) / 2, 42, draw::TEXT, "Applying update...", true);
            frame_end();
            for (int i = 0; i < 90; i++) svcSleepThread(10000000ull);
            httpd_stop();
            char err[192] = {0};
            if (update_apply(err, sizeof(err))) {
                g_quit = true;
                break;
            }
            set_toast("Update failed: %s", err);
            httpd_start(httpd_port());
        }

        C7sAppSnapshot s;
        app_snapshot(&s);
        const UsbCompositeStatus *u = usb_composite_status();

        if (s.running && g_screen != SCREEN_RUNNING) g_screen = SCREEN_RUNNING;
        if (g_screen == SCREEN_RUNNING && !s.running && s.finished) g_screen = SCREEN_RESULT;
        if (ticks_ms() - g_last_refresh > 2000) refresh_entries(false);

        Input in = poll_input();
        handle_input(in, s);

        frame_begin();
        switch (g_screen) {
            case SCREEN_HOME: draw_home(s, u); break;
            case SCREEN_CONFIRM: draw_confirm(s, u); break;
            case SCREEN_RUNNING: draw_running(s, u); break;
            case SCREEN_RESULT: draw_result(s, u); break;
            case SCREEN_SETTINGS: draw_settings(s, u); break;
        }
        draw_toast();
        frame_end();
        svcSleepThread(16000000ull);
    }

    frame_begin();
    draw::rect(0, 0, W, H, draw::BG, 0);
    ImVec2 cts = draw::text_size(42, "Closing...", true);
    draw::text((W - cts.x) / 2, (H - cts.y) / 2, 42, draw::TEXT, "Closing...", true);
    frame_end();

    app_exit_trace("ui: closing");
    httpd_stop();
    app_exit_trace("ui: httpd stopped");
    usb_worker_stop();
    app_exit_trace("ui: worker stopped");
    gfx_shutdown();
    app_exit_trace("ui: gfx shutdown");
    return 0;
}
