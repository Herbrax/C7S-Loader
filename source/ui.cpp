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
#include "carry.h"
#include "c7s.h"
#include "debuglog.h"
#include "httpd.h"
#include "ldn_route.h"
#include "library.h"
#include "live.h"
#include "record.h"
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
    bool l = false, r = false, zl = false, zr = false;
    bool plus_held = false;
    bool touch = false;
    bool touch_down = false, touch_up = false, touch_held = false;
    float tx = 0, ty = 0;
};

struct Hint {
    Action a;
    const char *text;
};

// ---- layout ----
constexpr float TABS_Y = 28.0f;
constexpr float CONTENT_Y = TOP + 24.0f;
constexpr float CONTENT_H = H - BOTTOM - CONTENT_Y - 24.0f;
constexpr float COL_GAP = 24.0f;
constexpr float SEND_LIST_W = 1120.0f;

enum Tab { TAB_SEND = 0, TAB_LIVE = 1, TAB_CONNECT = 2, TAB_LDN = 3 };
enum Overlay { OV_NONE, OV_RUNNING, OV_RESULT };
enum LiveView { LV_KBD, LV_PAD };
enum Mode { MODE_TEXT, MODE_SCRIPT, MODE_CHAT, MODE_UNKNOWN };

NWindow *g_win = nullptr;
Framebuffer g_fb;
bool g_fb_ready = false;
PadState g_pad;
bool g_touch_ready = false;
uint64_t g_last_frame = 0;
uint64_t g_prev_buttons = 0;
bool g_prev_touch = false;
uint64_t g_plus_start = 0;

Tab g_tab = TAB_SEND;
Overlay g_overlay = OV_NONE;
LiveView g_liveview = LV_KBD;

C7sEntry g_entries[64];
Mode g_modes[64];
int g_count = 0;
int g_cursor = 0;
int g_first = 0;
uint64_t g_last_refresh = 0;
bool g_quit = false;
char g_toast[192] = {0};
uint64_t g_toast_until = 0;

char g_preview[8][160];
int g_preview_count = 0;
char g_preview_name[160] = {0};

Rect g_tab_rects[4];
Rect g_ldn_start_rect;
Rect g_ldn_log_btn;
bool g_ldn_log_full = false;
int g_ldn_log_scroll = 0;
Rect g_row_rects[64];
Rect g_start_rect;
Rect g_refresh_rect;
Rect g_cancel_rect;
Rect g_back_rect;
Rect g_again_rect;
Rect g_port_rect;
Rect g_ktest_rect;
Rect g_kbdtab_rect;
Rect g_padtab_rect;

enum LiveKind { LK_KEY, LK_MOD, LK_PAD, LK_HAT, LK_SHORTCUT, LK_CARRY, LK_RELEASE, LK_RECORD };
struct LiveKey {
    Rect r;
    const char *label;
    LiveKind kind;
    uint8_t mod;
    uint8_t key;
    uint16_t bit;
    uint8_t hat;
};
LiveKey g_live_keys[128];
int g_live_key_count = 0;
int g_live_active = -1;
uint8_t g_live_mods = 0;
C7sPadReport g_live_pad;

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
    if (R_FAILED(framebufferCreate(&g_fb, g_win, (u32)W, (u32)H, PIXEL_FORMAT_RGBA_8888, 2))) return false;
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
    platform::font_regular = io.Fonts->AddFontFromMemoryTTF((void *)font_regular_ttf, (int)font_regular_ttf_size, 40.0f, &cfg);
    platform::font_bold = io.Fonts->AddFontFromMemoryTTF((void *)font_bold_ttf, (int)font_bold_ttf_size, 52.0f, &cfg);
    platform::font_mono = io.Fonts->AddFontFromMemoryTTF((void *)font_mono_ttf, (int)font_mono_ttf_size, 52.0f, &cfg);
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
    raster::render(data, raster::Target{dst, (int)W, (int)H, (int)(stride / 4)}, IM_COL32(11, 18, 32, 255));
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
    in.l = (down & HidNpadButton_L) != 0;
    in.r = (down & HidNpadButton_R) != 0;
    in.zl = (down & HidNpadButton_ZL) != 0;
    in.zr = (down & HidNpadButton_ZR) != 0;
    in.plus = (down & HidNpadButton_Plus) != 0;
    in.plus_held = (cur & HidNpadButton_Plus) != 0;
    in.minus = (down & HidNpadButton_Minus) != 0;

    HidTouchScreenState ts{};
    bool has_touch = g_touch_ready && hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    if (has_touch) {
        in.touch = !g_prev_touch;
        in.touch_down = in.touch;
        in.touch_held = true;
        in.tx = (float)ts.touches[0].x * (W / 1280.0f);
        in.ty = (float)ts.touches[0].y * (H / 720.0f);
    } else {
        in.touch_up = g_prev_touch;
    }
    g_prev_touch = has_touch;
    carry_update_pad(&g_pad);
    return in;
}

Mode mode_of(const char *path, const C7sEntry &e) {
    C7sPayload p;
    Mode m = MODE_UNKNOWN;
    if (c7s_parse_file(path, &p)) {
        if (p.plain) m = MODE_TEXT;
        else if (p.script) m = MODE_SCRIPT;
        else m = MODE_CHAT;
        c7s_free(&p);
    } else {
        m = e.header ? MODE_TEXT : MODE_UNKNOWN;
    }
    return m;
}

void refresh_preview() {
    g_preview_count = 0;
    g_preview_name[0] = 0;
    if (g_count <= 0 || g_cursor < 0 || g_cursor >= g_count) return;
    const C7sEntry &e = g_entries[g_cursor];
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, e.name) >= (int)sizeof(path)) return;
    snprintf(g_preview_name, sizeof(g_preview_name), "%s", e.name);
    C7sPayload p;
    if (!c7s_parse_file(path, &p)) return;
    int n = p.line_count < 8 ? p.line_count : 8;
    for (int i = 0; i < n; i++) {
        snprintf(g_preview[i], sizeof(g_preview[i]), "%s", p.lines[i] ? p.lines[i] : "");
        g_preview[i][sizeof(g_preview[i]) - 1] = 0;
    }
    g_preview_count = n;
    c7s_free(&p);
}

void refresh_entries(bool prefer_selected) {
    char keep[128] = {0};
    if (g_count > 0 && g_cursor >= 0 && g_cursor < g_count) snprintf(keep, sizeof(keep), "%s", g_entries[g_cursor].name);

    C7sEntry fresh[64];
    int n = c7s_list(fresh, 64);
    memcpy(g_entries, fresh, sizeof(C7sEntry) * (size_t)n);
    g_count = n;
    for (int i = 0; i < n; i++) {
        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, g_entries[i].name) < (int)sizeof(path)) g_modes[i] = mode_of(path, g_entries[i]);
        else g_modes[i] = MODE_UNKNOWN;
    }

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
    refresh_preview();
}

// ---------- shared widgets ----------
void mode_tag(float x, float y, Mode m) {
    const char *label = m == MODE_TEXT ? "TEXT" : m == MODE_SCRIPT ? "SCRIPT" : m == MODE_CHAT ? "CIV 7 CHAT" : "UNREADABLE";
    ImU32 fg = m == MODE_TEXT ? IM_COL32(143, 193, 255, 255)
               : m == MODE_SCRIPT ? IM_COL32(126, 230, 184, 255)
               : m == MODE_CHAT ? IM_COL32(236, 210, 145, 255)
                                : IM_COL32(255, 176, 176, 255);
    ImU32 bg = m == MODE_TEXT ? IM_COL32(47, 140, 255, 40)
               : m == MODE_SCRIPT ? IM_COL32(60, 207, 145, 38)
               : m == MODE_CHAT ? IM_COL32(217, 182, 94, 40)
                                : IM_COL32(255, 107, 107, 38);
    ImVec2 ts = draw::text_size(20, label, true);
    float w = ts.x + 24, h = 30;
    draw::rect(x, y, w, h, bg, 7);
    draw::text(x + 12, y + (h - ts.y) / 2, 20, fg, label, true);
}
float mode_tag_w(Mode m) {
    const char *label = m == MODE_TEXT ? "TEXT" : m == MODE_SCRIPT ? "SCRIPT" : m == MODE_CHAT ? "CIV 7 CHAT" : "UNREADABLE";
    return draw::text_size(20, label, true).x + 24;
}
ImU32 mode_icon_col(Mode m) {
    return m == MODE_TEXT ? IM_COL32(143, 193, 255, 255)
           : m == MODE_SCRIPT ? IM_COL32(126, 230, 184, 255)
           : m == MODE_CHAT ? IM_COL32(236, 210, 145, 255)
                            : IM_COL32(255, 176, 176, 255);
}

void toggle_switch(float x, float y, bool on) {
    float w = 64, h = 36;
    draw::rect(x, y, w, h, on ? draw::OK : draw::LINE, h / 2);
    float r = h / 2 - 5;
    float cx = on ? x + w - h / 2 : x + h / 2;
    draw::dl()->AddCircleFilled(ImVec2(cx, y + h / 2), r, draw::WHITE, 20);
}

void draw_top(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw::wordmark(MARGIN, 24, 48);

    struct T { const char *label; } tabs[4] = {{"Send"}, {"Live input"}, {"Status"}, {"LDN"}};
    float tw[4];
    float total = 0;
    for (int i = 0; i < 4; i++) {
        tw[i] = draw::text_size(32, tabs[i].label, true).x + 72;
        total += tw[i];
    }
    total += 3 * 16;
    float x = (W - total) / 2;
    for (int i = 0; i < 4; i++) {
        bool on = (int)g_tab == i;
        g_tab_rects[i] = {x, TABS_Y, tw[i], 56};
        if (on) {
            draw::rect(x, TABS_Y, tw[i], 56, draw::PANEL2, 28);
            draw::frame(x, TABS_Y, tw[i], 56, draw::LINE, 28, 1.5f);
        }
        ImVec2 ts = draw::text_size(32, tabs[i].label, true);
        draw::text(x + (tw[i] - ts.x) / 2, TABS_Y + (56 - ts.y) / 2, 32, on ? draw::TEXT : draw::MUTED, tabs[i].label, true);
        x += tw[i] + 16;
    }

    bool usb_ok = u && u->initialized && u->pad_ready && u->kbd_ready;
    const char *usb_txt = (!u || !u->initialized) ? "Plug in USB" : usb_ok ? "Switch 2 linked" : "USB waiting";
    ImU32 dot = (!u || !u->initialized) ? draw::ERR : usb_ok ? draw::OK : draw::ACCENT2;
    float cw = draw::text_size(22, usb_txt, true).x + 50;
    float cx = W - MARGIN - cw;
    draw::pill(cx, 34, usb_txt, dot);

    if (record_active()) {
        const char *rc = "REC";
        float rw = draw::text_size(22, rc, true).x + 50;
        draw::pill(cx - rw - 12, 34, rc, draw::RED_HOT);
    }
}

void draw_bottom(const Hint *hints, int n) {
    draw::rect(0, H - BOTTOM, W, BOTTOM, draw::PANEL, 0);
    draw::rect(0, H - BOTTOM, W, 1.5f, draw::LINE, 0);
    float x = MARGIN;
    float y = H - BOTTOM + (BOTTOM - 44) / 2 - 2;
    for (int i = 0; i < n; i++) x += draw::hint(x, y, hints[i].a, hints[i].text);
    const char *exit_txt = "Hold + to exit";
    float ew = draw::text_size(30, exit_txt).x;
    draw::text(W - MARGIN - ew, H - BOTTOM + (BOTTOM - 30) / 2 - 1, 30, draw::MUTED, exit_txt);
}

void draw_toast() {
    if (!g_toast[0] || ticks_ms() >= g_toast_until) return;
    ImVec2 ts = draw::text_size(28, g_toast, true);
    float w = ts.x + 56, h = 64;
    float x = (W - w) / 2, y = TOP + 12;
    draw::rect(x, y, w, h, draw::PANEL2, 14);
    draw::frame(x, y, w, h, draw::ACCENT, 14, 2);
    draw::text(x + 28, y + (h - ts.y) / 2, 28, draw::TEXT, g_toast, true);
}

void format_size(long long n, char *buf, size_t len) {
    if (n < 1024) snprintf(buf, len, "%lld B", n);
    else if (n < 1024 * 1024) snprintf(buf, len, "%.1f KB", (double)n / 1024.0);
    else snprintf(buf, len, "%.1f MB", (double)n / (1024.0 * 1024.0));
}

void web_url(const C7sAppSnapshot &s, char *buf, size_t len) {
    snprintf(buf, len, "http://%s:%u", s.ip[0] && strcmp(s.ip, "unknown") ? s.ip : "console", (unsigned)s.port);
}

// ---------- SEND ----------
void draw_send(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    float lx = MARGIN, ly = CONTENT_Y, lw = SEND_LIST_W, lh = CONTENT_H;
    draw::panel(lx, ly, lw, lh);
    draw::text(lx + 28, ly + 22, 26, draw::MUTED, "SCRIPTS ON SD CARD", true);

    char cnt[32];
    snprintf(cnt, sizeof(cnt), "%d files", g_count);
    float cntw = draw::text_size(24, cnt).x;
    g_refresh_rect = {lx + lw - 170, ly + 18, 150, 44};
    draw::rect(g_refresh_rect.x, g_refresh_rect.y, g_refresh_rect.w, g_refresh_rect.h, draw::PANEL2, 10);
    draw::frame(g_refresh_rect.x, g_refresh_rect.y, g_refresh_rect.w, g_refresh_rect.h, draw::LINE, 10, 1.5f);
    ImVec2 rts = draw::text_size(24, "Refresh", true);
    draw::text(g_refresh_rect.x + (g_refresh_rect.w - rts.x) / 2, g_refresh_rect.y + (g_refresh_rect.h - rts.y) / 2, 24, draw::TEXT, "Refresh", true);
    draw::text(g_refresh_rect.x - 20 - cntw, ly + 24, 24, draw::MUTED, cnt);

    const float row_top = 86.0f, row_h = 104.0f, row_gap = 12.0f;
    int visible = (int)((lh - row_top - 16) / (row_h + row_gap));
    if (visible < 1) visible = 1;
    if (g_cursor < g_first) g_first = g_cursor;
    if (g_cursor >= g_first + visible) g_first = g_cursor - visible + 1;
    if (g_first < 0) g_first = 0;

    for (int i = 0; i < 64; i++) g_row_rects[i] = Rect{};

    if (g_count == 0) {
        const char *a = "No scripts yet";
        const char *b = "Drop a .C7S from the web app, or record one from Live input.";
        ImVec2 ta = draw::text_size(36, a, true);
        ImVec2 tb = draw::text_size(26, b);
        float cx = lx + lw / 2, cy = ly + lh / 2 - 40;
        draw::text(cx - ta.x / 2, cy, 36, draw::TEXT, a, true);
        draw::text(cx - tb.x / 2, cy + 52, 26, draw::MUTED, b);
    }

    for (int i = 0; i < visible; i++) {
        int idx = g_first + i;
        if (idx >= g_count) break;
        C7sEntry &e = g_entries[idx];
        Rect r{lx + 16, ly + row_top + i * (row_h + row_gap), lw - 32, row_h};
        g_row_rects[idx] = r;
        bool sel = idx == g_cursor;
        draw::rect(r.x, r.y, r.w, r.h, sel ? draw::SEL : draw::PANEL2, 14);
        draw::frame(r.x, r.y, r.w, r.h, sel ? draw::ACCENT : draw::LINE, 14, sel ? 2.5f : 1.2f);

        draw::file_icon(r.x + 20, r.y + 28, 48, mode_icon_col(g_modes[idx]));
        float tx = r.x + 84;
        float tagw = mode_tag_w(g_modes[idx]);
        char sz[32];
        format_size(e.size, sz, sizeof(sz));
        float szw = draw::text_size(22, sz).x;
        float right_pad = tagw + szw + 48;
        draw::text_clip(tx, r.y + 22, 32, draw::TEXT, e.name, r.w - 84 - right_pad, true);
        char meta[220];
        if (e.header) snprintf(meta, sizeof(meta), "%s - %d lines", e.title, e.lines);
        else snprintf(meta, sizeof(meta), "no header - %d lines", e.lines);
        draw::text_clip(tx, r.y + 60, 23, draw::MUTED, meta, r.w - 84 - right_pad);

        float tagx = r.x + r.w - 20 - szw - 16 - tagw;
        mode_tag(tagx, r.y + (r.h - 30) / 2, g_modes[idx]);
        draw::text(r.x + r.w - 20 - szw, r.y + (r.h - 24) / 2, 22, draw::MUTED, sz);
    }

    if (g_first > 0) draw::text(lx + lw / 2 - 6, ly + row_top - 24, 22, draw::MUTED, "^");
    if (g_first + visible < g_count) draw::text(lx + lw / 2 - 6, ly + lh - 28, 22, draw::MUTED, "v");

    // detail panel
    float dx = lx + lw + COL_GAP, dy = CONTENT_Y, dw = W - MARGIN - dx, dh = CONTENT_H;
    draw::panel(dx, dy, dw, dh);
    draw::text(dx + 28, dy + 22, 26, draw::MUTED, "SELECTED", true);
    g_start_rect = Rect{};

    if (g_count == 0) {
        draw::text(dx + 28, dy + 70, 28, draw::MUTED, "Nothing to send yet.");
        char url[160];
        web_url(s, url, sizeof(url));
        draw::text(dx + 28, dy + 120, 26, draw::MUTED, "Open the web app:");
        draw::text_clip(dx + 28, dy + 154, 28, draw::ACCENT, url, dw - 56);
    } else {
        C7sEntry &e = g_entries[g_cursor];
        Mode m = g_modes[g_cursor];
        float x = dx + 28, y = dy + 62;
        mode_tag(x, y, m);
        y += 44;
        draw::text_clip(x, y, 38, draw::TEXT, e.name, dw - 56, true);
        y += 50;
        char sz[32];
        format_size(e.size, sz, sizeof(sz));
        char factline[256];
        if (e.header) snprintf(factline, sizeof(factline), "%d lines    %s    \"%s\"", e.lines, sz, e.title);
        else snprintf(factline, sizeof(factline), "%d lines    %s", e.lines, sz);
        draw::text_clip(x, y, 24, draw::MUTED, factline, dw - 56);
        y += 34;
        if (e.mismatch) {
            char w[160];
            snprintf(w, sizeof(w), "Header says %d lines but the file has %d.", e.declared, e.lines);
            draw::text_clip(x, y, 22, draw::ACCENT2, w, dw - 56);
            y += 28;
        }
        y += 6;

        // preview box
        float pbx = x, pby = y, pbw = dw - 56, pbh = 240;
        draw::rect(pbx, pby, pbw, pbh, IM_COL32(10, 16, 28, 255), 12);
        draw::frame(pbx, pby, pbw, pbh, draw::LINE, 12, 1.2f);
        float py = pby + 14;
        draw::dl()->PushClipRect(ImVec2(pbx + 2, pby + 2), ImVec2(pbx + pbw - 2, pby + pbh - 2), true);
        for (int i = 0; i < g_preview_count && py < pby + pbh - 24; i++) {
            char num[8];
            snprintf(num, sizeof(num), "%d", i + 1);
            draw::dl()->AddText(platform::font_mono, 22, ImVec2(pbx + 14, py), draw::MUTED, num);
            draw::dl()->AddText(platform::font_mono, 22, ImVec2(pbx + 54, py), IM_COL32(185, 197, 217, 255), g_preview[i]);
            py += 30;
        }
        draw::dl()->PopClipRect();
        y = pby + pbh + 22;

        const char *expl = m == MODE_TEXT ? "Types each line, then presses Enter."
                           : m == MODE_CHAT ? "Opens Civ 7 chat with the gamepad, types each line, and sends it."
                           : m == MODE_SCRIPT ? "Replays the recorded key and button timings."
                                              : "This file could not be parsed.";
        draw::text_clip(x, y, 24, draw::MUTED, expl, dw - 56);

        float bw = dw - 56, bh = 112;
        float bx = dx + 28, by = dy + dh - 28 - bh;
        bool usb_ok = u && u->initialized && u->pad_ready && u->kbd_ready;
        draw::rect(bx, by, bw, bh, usb_ok ? draw::ACCENT : draw::PANEL2, 16);
        if (!usb_ok) draw::frame(bx, by, bw, bh, draw::LINE, 16, 1.5f);
        const char *blabel = "Send to Switch 2";
        ImVec2 bts = draw::text_size(40, blabel, true);
        draw::button(bx + bw / 2 - bts.x / 2 - 60, by + (bh - 46) / 2, 46, ACT_CROSS);
        draw::text(bx + bw / 2 - bts.x / 2, by + (bh - bts.y) / 2, 40, usb_ok ? draw::WHITE : draw::MUTED, blabel, true);
        g_start_rect = {bx, by, bw, bh};
    }

    Hint hints[] = {{ACT_CROSS, "Send"}, {ACT_TRIANGLE, "Refresh"}, {ACT_L1, ""}, {ACT_R1, "Switch tab"}};
    draw_bottom(hints, 4);
}

// ---------- RUNNING / RESULT overlays ----------
void draw_running(const C7sAppSnapshot &s) {
    draw::dl()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(4, 8, 16, 200));
    bool counting = s.total <= 0;
    const float cw = 1200, ch = 620;
    const float cx = (W - cw) / 2, cy = (H - ch) / 2;
    draw::panel(cx, cy, cw, ch);

    if (counting) {
        draw::text(cx + 48, cy + 44, 28, draw::MUTED, "STARTING", true);
        const char *big = "Switch to the Switch 2 now";
        ImVec2 bt = draw::text_size(52, big, true);
        draw::text(cx + (cw - bt.x) / 2, cy + 180, 52, draw::TEXT, big, true);
        const char *sub = "Open the chat or text field you want to type into.";
        ImVec2 st = draw::text_size(30, sub);
        draw::text(cx + (cw - st.x) / 2, cy + 256, 30, draw::MUTED, sub);
        char line[220];
        snprintf(line, sizeof(line), "Sending %s", s.selected[0] ? s.selected : "payload");
        ImVec2 lt = draw::text_size(28, line, true);
        draw::text(cx + (cw - lt.x) / 2, cy + 300, 28, draw::ACCENT, line, true);
        draw::spinner(cx + cw / 2, cy + 110, 26, draw::ACCENT);
    } else {
        draw::text(cx + 48, cy + 44, 28, draw::MUTED, "SENDING TO SWITCH 2", true);
        draw::text_clip(cx + 48, cy + 84, 44, draw::TEXT, s.selected[0] ? s.selected : "payload", cw - 96, true);
        float pct = s.total > 0 ? (float)s.step / (float)s.total : 0;
        if (pct > 1) pct = 1;
        char big[32];
        snprintf(big, sizeof(big), "%d%%", (int)(pct * 100));
        draw::text(cx + 48, cy + 200, 90, draw::TEXT, big, true);
        char stp[64];
        snprintf(stp, sizeof(stp), "step %d / %d", s.step, s.total);
        draw::text(cx + 260, cy + 244, 32, draw::MUTED, stp);
        float bx = cx + 48, by = cy + 330, bw = cw - 96, bh = 26;
        draw::rect(bx, by, bw, bh, draw::PANEL2, bh / 2);
        if (pct > 0) draw::rect(bx, by, bw * pct, bh, draw::ACCENT, bh / 2);
        draw::text(cx + 48, cy + 400, 26, draw::MUTED, "Leave both consoles alone and keep the cable plugged in.");
    }

    float bw = 360, bh = 92;
    float bx = cx + (cw - bw) / 2, by = cy + ch - 48 - bh;
    draw::red_button(bx, by, bw, bh, "STOP", true);
    g_cancel_rect = {bx, by, bw, bh};

    Hint hints[] = {{ACT_CIRCLE, "Stop"}};
    draw_bottom(hints, 1);
}

void draw_result(const C7sAppSnapshot &s) {
    draw::dl()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(4, 8, 16, 200));
    const float cw = 1000, ch = 540;
    const float cx = (W - cw) / 2, cy = (H - ch) / 2;
    draw::panel(cx, cy, cw, ch);

    float orbr = 72;
    float ocx = cx + cw / 2, ocy = cy + 150;
    draw::dl()->AddCircleFilled(ImVec2(ocx, ocy), orbr, s.ok ? IM_COL32(60, 207, 145, 40) : IM_COL32(255, 107, 107, 40), 32);
    const char *big = s.ok ? "Sent" : "Stopped";
    ImVec2 bt = draw::text_size(54, big, true);
    draw::text(cx + (cw - bt.x) / 2, cy + 250, 54, s.ok ? draw::OK : draw::ERR, big, true);
    ImVec2 mt = draw::text_size(28, s.message);
    draw::text_clip(cx + (cw - mt.x) / 2 < cx + 40 ? cx + 40 : cx + (cw - mt.x) / 2, cy + 322, 28, draw::MUTED, s.message, cw - 80);
    ImVec2 nt = draw::text_size(26, s.selected);
    draw::text_clip(cx + (cw - nt.x) / 2 < cx + 40 ? cx + 40 : cx + (cw - nt.x) / 2, cy + 360, 26, draw::MUTED, s.selected, cw - 80);

    float bw = 300, bh = 86, gap = 28;
    float totw = bw * 2 + gap;
    float bx = cx + (cw - totw) / 2, by = cy + ch - 48 - bh;
    draw::accent_button(bx, by, bw, bh, "Back", false);
    g_back_rect = {bx, by, bw, bh};
    draw::accent_button(bx + bw + gap, by, bw, bh, "Send again", true);
    g_again_rect = {bx + bw + gap, by, bw, bh};

    Hint hints[] = {{ACT_CIRCLE, "Back"}, {ACT_CROSS, "Send again"}};
    draw_bottom(hints, 2);
}

// ---------- CONNECT ----------
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

void kv_row(float x, float y, float w, const char *k, const char *v, ImU32 vcol) {
    draw::text(x, y, 26, draw::MUTED, k);
    ImVec2 vt = draw::text_size(26, v, true);
    draw::text(x + w - vt.x, y, 26, vcol, v, true);
    draw::rect(x, y + 44, w, 1.2f, draw::LINE, 0);
}

void draw_connect(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    float lw = 1080, lx = MARGIN, ly = CONTENT_Y, lh = CONTENT_H;
    draw::panel(lx, ly, lw, lh);
    draw::text(lx + 28, ly + 22, 26, draw::MUTED, "WEB APP", true);
    draw::text(lx + 28, ly + 74, 36, draw::TEXT, "Open on a phone or PC", true);
    char url[160];
    web_url(s, url, sizeof(url));
    draw::text_clip(lx + 28, ly + 128, 56, httpd_running() ? draw::ACCENT : draw::ERR,
                    httpd_running() ? url : "web server offline", lw - 56);
    draw::text(lx + 28, ly + 200, 26, draw::MUTED, "The device must be on the same Wi-Fi as this Switch.");
    draw::text(lx + 28, ly + 240, 26, draw::MUTED, "Upload scripts, type from a real keyboard, record macros.");

    g_port_rect = {lx + 28, ly + lh - 110, 320, 72};
    draw::accent_button(g_port_rect.x, g_port_rect.y, g_port_rect.w, g_port_rect.h, "Change port", false);
    char pl[48];
    snprintf(pl, sizeof(pl), "Port %u", (unsigned)s.port);
    draw::text(g_port_rect.x + g_port_rect.w + 24, g_port_rect.y + (72 - 28) / 2, 28, draw::MUTED, pl);

    float rx = lx + lw + COL_GAP, ry = CONTENT_Y, rw = W - MARGIN - rx, rh = CONTENT_H;
    draw::panel(rx, ry, rw, rh);
    draw::text(rx + 28, ry + 22, 26, draw::MUTED, "USB LINK TO SWITCH 2", true);
    bool linked = u && u->initialized && u->pad_ready && u->kbd_ready;
    float ox = rx + 28, oy = ry + 66;
    draw::dl()->AddCircleFilled(ImVec2(ox + 30, oy + 30), 30, linked ? IM_COL32(60, 207, 145, 40) : IM_COL32(255, 196, 64, 40), 24);
    draw::text(ox + 76, oy + 2, 36, draw::TEXT, linked ? "Linked" : (u && u->initialized ? "Waiting" : "Not started"), true);
    draw::text(ox + 76, oy + 44, 22, draw::MUTED, linked ? "Ready to send keys and buttons" : (u && u->stage[0] ? u->stage : "check USB log"));

    float kx = rx + 28, ky = ry + 170, kw = rw - 56;
    kv_row(kx, ky, kw, "Gamepad", (u && u->pad_ready) ? "Ready" : "Waiting", (u && u->pad_ready) ? draw::OK : draw::ACCENT2);
    kv_row(kx, ky + 56, kw, "Keyboard", (u && u->kbd_ready) ? "Ready" : "Waiting", (u && u->kbd_ready) ? draw::OK : draw::ACCENT2);
    char st[32];
    snprintf(st, sizeof(st), "%d", u ? (int)u->state : -1);
    kv_row(kx, ky + 112, kw, "Connection state", (u && u->state == 5) ? "Configured" : st, draw::TEXT);
    char fr[32];
    snprintf(fr, sizeof(fr), "%u", u ? (unsigned)u->reports_failed : 0);
    kv_row(kx, ky + 168, kw, "Failed reports", fr, draw::TEXT);

    g_ktest_rect = {rx + 28, ry + rh - 110, rw - 56, 72};
    draw::accent_button(g_ktest_rect.x, g_ktest_rect.y, g_ktest_rect.w, g_ktest_rect.h, "Test keyboard - types \"hello\"", false);

    Hint hints[] = {{ACT_TRIANGLE, "Port"}, {ACT_SQUARE, "Test keyboard"}, {ACT_L1, ""}, {ACT_R1, "Switch tab"}};
    draw_bottom(hints, 4);
}

// ---------- LIVE ----------
void live_neutral() {
    memset(&g_live_pad, 0, sizeof(g_live_pad));
    g_live_pad.hat = SWITCH_HAT_NOTHING;
    g_live_pad.lx = g_live_pad.ly = g_live_pad.rx = g_live_pad.ry = SWITCH_STICK_MID;
}

void live_add(float x, float y, float w, float h, const char *label, LiveKind kind, uint8_t mod = 0, uint8_t key = 0, uint16_t bit = 0, uint8_t hat = SWITCH_HAT_NOTHING) {
    if (g_live_key_count >= (int)(sizeof(g_live_keys) / sizeof(g_live_keys[0]))) return;
    g_live_keys[g_live_key_count++] = LiveKey{Rect{x, y, w, h}, label, kind, mod, key, bit, hat};
}

void leave_live() {
    if (carry_enabled()) carry_set_enabled(false);
    live_release_all();
    live_set_enabled(false);
    g_live_mods = 0;
    g_live_active = -1;
}

void enter_live() {
    live_set_enabled(true);
    live_release_all();
    g_live_mods = 0;
    g_live_active = -1;
    live_neutral();
    live_set_pad(&g_live_pad);
}

void live_key_release() {
    if (g_live_active < 0) return;
    const LiveKey &k = g_live_keys[g_live_active];
    if (k.kind == LK_KEY || k.kind == LK_SHORTCUT) {
        live_set_key(g_live_mods, 0);
    } else if (k.kind == LK_PAD) {
        g_live_pad.buttons &= (uint16_t)~k.bit;
        live_set_pad(&g_live_pad);
    } else if (k.kind == LK_HAT) {
        g_live_pad.hat = SWITCH_HAT_NOTHING;
        live_set_pad(&g_live_pad);
    }
    g_live_active = -1;
}

void live_key_press(int i) {
    const LiveKey &k = g_live_keys[i];
    switch (k.kind) {
        case LK_KEY:
            g_live_active = i;
            live_set_key(g_live_mods, k.key);
            break;
        case LK_MOD:
            g_live_mods ^= k.mod;
            live_set_key(g_live_mods, 0);
            break;
        case LK_PAD:
            g_live_active = i;
            g_live_pad.buttons |= k.bit;
            live_set_pad(&g_live_pad);
            break;
        case LK_HAT:
            g_live_active = i;
            g_live_pad.hat = k.hat;
            live_set_pad(&g_live_pad);
            break;
        case LK_SHORTCUT:
            g_live_active = i;
            live_set_key(k.mod, k.key);
            break;
        case LK_CARRY:
            carry_set_enabled(!carry_enabled());
            break;
        case LK_RELEASE:
            live_release_all();
            g_live_mods = 0;
            live_neutral();
            live_set_pad(&g_live_pad);
            g_live_active = -1;
            break;
        case LK_RECORD:
            set_toast(record_toggle() ? "Recording presses..." : "Recording saved");
            break;
    }
}

// Does the physical controller currently hold the button this on-screen key
// represents? Lets a real Joy-Con press light up the drawn gamepad.
bool phys_live_active(const LiveKey &k) {
    // Only reflect the physical controller when its input is actually being
    // passed through; otherwise the Joy-Con is driving this app's menus, not
    // the virtual pad, and lighting it up would be misleading. With
    // pass-through off, only touchscreen presses light the buttons.
    if (!carry_enabled()) return false;
    uint64_t h = padGetButtons(&g_pad);
    if (k.kind == LK_PAD) {
        switch (k.bit) {
            case SWITCH_BTN_A: return (h & HidNpadButton_A) != 0;
            case SWITCH_BTN_B: return (h & HidNpadButton_B) != 0;
            case SWITCH_BTN_X: return (h & HidNpadButton_X) != 0;
            case SWITCH_BTN_Y: return (h & HidNpadButton_Y) != 0;
            case SWITCH_BTN_L: return (h & HidNpadButton_L) != 0;
            case SWITCH_BTN_R: return (h & HidNpadButton_R) != 0;
            case SWITCH_BTN_ZL: return (h & HidNpadButton_ZL) != 0;
            case SWITCH_BTN_ZR: return (h & HidNpadButton_ZR) != 0;
            case SWITCH_BTN_MINUS: return (h & HidNpadButton_Minus) != 0;
            case SWITCH_BTN_PLUS: return (h & HidNpadButton_Plus) != 0;
            case SWITCH_BTN_L3: return (h & HidNpadButton_StickL) != 0;
            case SWITCH_BTN_R3: return (h & HidNpadButton_StickR) != 0;
            default: return false;
        }
    }
    if (k.kind == LK_HAT) {
        switch (k.hat) {
            case SWITCH_HAT_UP: return (h & HidNpadButton_Up) != 0;
            case SWITCH_HAT_DOWN: return (h & HidNpadButton_Down) != 0;
            case SWITCH_HAT_LEFT: return (h & HidNpadButton_Left) != 0;
            case SWITCH_HAT_RIGHT: return (h & HidNpadButton_Right) != 0;
            default: return false;
        }
    }
    return false;
}

void draw_live_key(const LiveKey &k, int i) {
    bool active = (i == g_live_active) || (k.kind == LK_MOD && (g_live_mods & k.mod) != 0) || phys_live_active(k);
    bool round = (k.kind == LK_PAD && (k.bit == SWITCH_BTN_A || k.bit == SWITCH_BTN_B || k.bit == SWITCH_BTN_X || k.bit == SWITCH_BTN_Y));
    float rounding = round ? k.r.h / 2 : 12;
    draw::rect(k.r.x, k.r.y, k.r.w, k.r.h, active ? draw::ACCENT : draw::PANEL2, rounding);
    draw::frame(k.r.x, k.r.y, k.r.w, k.r.h, active ? draw::WHITE : draw::LINE, rounding, active ? 2.0f : 1.2f);
    float fs = strlen(k.label) > 6 ? 22.0f : 30.0f;
    ImVec2 ts = draw::text_size(fs, k.label, true);
    if (ts.x > k.r.w - 10) {
        draw::text_clip(k.r.x + 6, k.r.y + (k.r.h - ts.y) / 2, fs, active ? draw::WHITE : draw::TEXT, k.label, k.r.w - 10, true);
    } else {
        draw::text(k.r.x + (k.r.w - ts.x) / 2, k.r.y + (k.r.h - ts.y) / 2, fs, active ? draw::WHITE : draw::TEXT, k.label, true);
    }
}

void draw_live(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    (void)u;
    g_live_key_count = 0;
    float px = MARGIN, py = CONTENT_Y, pw = W - 2 * MARGIN, ph = CONTENT_H;

    // toolbar
    float ty = py;
    // segment Keyboard | Gamepad
    float segw = 460, segh = 72;
    draw::rect(px, ty, segw, segh, draw::PANEL2, 16);
    draw::frame(px, ty, segw, segh, draw::LINE, 16, 1.2f);
    g_kbdtab_rect = {px + 6, ty + 6, segw / 2 - 6, segh - 12};
    g_padtab_rect = {px + segw / 2, ty + 6, segw / 2 - 6, segh - 12};
    if (g_liveview == LV_KBD) { draw::rect(g_kbdtab_rect.x, g_kbdtab_rect.y, g_kbdtab_rect.w, g_kbdtab_rect.h, IM_COL32(33, 45, 72, 255), 12); }
    else { draw::rect(g_padtab_rect.x, g_padtab_rect.y, g_padtab_rect.w, g_padtab_rect.h, IM_COL32(33, 45, 72, 255), 12); }
    ImVec2 k1 = draw::text_size(28, "Keyboard", true);
    draw::text(g_kbdtab_rect.x + (g_kbdtab_rect.w - k1.x) / 2, g_kbdtab_rect.y + (g_kbdtab_rect.h - k1.y) / 2, 28, g_liveview == LV_KBD ? draw::TEXT : draw::MUTED, "Keyboard", true);
    ImVec2 k2 = draw::text_size(28, "Gamepad", true);
    draw::text(g_padtab_rect.x + (g_padtab_rect.w - k2.x) / 2, g_padtab_rect.y + (g_padtab_rect.h - k2.y) / 2, 28, g_liveview == LV_PAD ? draw::TEXT : draw::MUTED, "Gamepad", true);

    // right-aligned controls: carry toggle, record, release
    float rel_w = 190, rec_w = 210, carry_w = 420;
    float cx = px + pw - rel_w;
    live_add(cx, ty, rel_w, segh, "Release all", LK_RELEASE);
    cx -= rec_w + 14;
    live_add(cx, ty, rec_w, segh, record_active() ? "Stop recording" : "Record", LK_RECORD);
    cx -= carry_w + 14;
    live_add(cx, ty, carry_w, segh, "", LK_CARRY);
    float carry_x = cx;

    float body_y = ty + segh + 20;

    // pass-through banner
    if (carry_enabled()) {
        float bh = 60;
        draw::rect(px, body_y, pw, bh, IM_COL32(217, 182, 94, 32), 12);
        draw::frame(px, body_y, pw, bh, IM_COL32(217, 182, 94, 140), 12, 1.5f);
        draw::text(px + 20, body_y + (bh - 27) / 2, 27, draw::GOLD, "Joy-Con pass-through is on - use the touchscreen to control this app.", true);
        body_y += bh + 16;
    }

    if (g_liveview == LV_KBD) {
        draw::panel(px, body_y, pw, ph - (body_y - py));
        float kx = px + 20, ky = body_y + 24;
        float kw = (pw - 40 - 13 * 10) / 14.0f;
        float kh = 92, kg = 10;

        static const char *r1l[] = {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Back"};
        static const uint8_t r1k[] = {53, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42};
        static const char *r2l[] = {"Tab", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "\\"};
        static const uint8_t r2k[] = {43, 20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 49};
        static const char *r3l[] = {"Esc", "a", "s", "d", "f", "g", "h", "j", "k", "l", ";", "'", "Enter", ""};
        static const uint8_t r3k[] = {41, 4, 22, 7, 9, 10, 11, 13, 14, 15, 51, 52, 40, 0};
        static const char *r4l[] = {"z", "x", "c", "v", "b", "n", "m", ",", ".", "/", "Up", "Left", "Down", "Right"};
        static const uint8_t r4k[] = {29, 27, 6, 25, 5, 17, 16, 54, 55, 56, 82, 80, 81, 79};

        auto row = [&](const char *labels[], const uint8_t keys[], int n, float y) {
            for (int i = 0; i < n; i++) {
                if (!labels[i][0]) continue;
                live_add(kx + i * (kw + kg), y, kw, kh, labels[i], LK_KEY, 0, keys[i]);
            }
        };
        row(r1l, r1k, 14, ky);
        row(r2l, r2k, 14, ky + (kh + kg));
        row(r3l, r3k, 13, ky + 2 * (kh + kg));
        row(r4l, r4k, 14, ky + 3 * (kh + kg));

        float y5 = ky + 4 * (kh + kg);
        live_add(kx, y5, kw * 2 + kg, kh, "Ctrl", LK_MOD, 1);
        live_add(kx + 2 * (kw + kg), y5, kw * 2 + kg, kh, "Alt", LK_MOD, 4);
        live_add(kx + 4 * (kw + kg), y5, kw * 2 + kg, kh, "Shift", LK_MOD, 2);
        live_add(kx + 6 * (kw + kg), y5, kw * 4 + 3 * kg, kh, "Space", LK_KEY, 0, 44);
        live_add(kx + 10 * (kw + kg), y5, kw * 2 + kg, kh, "Alt+Enter", LK_SHORTCUT, 4, 40);
        live_add(kx + 12 * (kw + kg), y5, kw * 2 + kg, kh, "Alt+Esc", LK_SHORTCUT, 4, 41);
    } else {
        draw::panel(px, body_y, pw, ph - (body_y - py));
        float cxp = px + pw / 2;
        // dpad left
        float dcx = px + 220, dcy = body_y + 230;
        float db = 96;
        live_add(dcx - db / 2, dcy - db - 6, db, db, "Up", LK_HAT, 0, 0, 0, SWITCH_HAT_UP);
        live_add(dcx - db - 6 - db / 2, dcy, db, db, "Lt", LK_HAT, 0, 0, 0, SWITCH_HAT_LEFT);
        live_add(dcx + 6 + db / 2, dcy, db, db, "Rt", LK_HAT, 0, 0, 0, SWITCH_HAT_RIGHT);
        live_add(dcx - db / 2, dcy + db + 6, db, db, "Dn", LK_HAT, 0, 0, 0, SWITCH_HAT_DOWN);
        draw::text(dcx - 60, dcy + 2 * db + 24, 22, draw::MUTED, "D-PAD");

        // face buttons right
        float fcx = px + pw - 240, fcy = body_y + 230;
        float fb = 120;
        live_add(fcx - fb / 2, fcy - fb - 6, fb, fb, "X", LK_PAD, 0, 0, SWITCH_BTN_X);
        live_add(fcx - fb - 6 - fb / 2, fcy, fb, fb, "Y", LK_PAD, 0, 0, SWITCH_BTN_Y);
        live_add(fcx + 6 + fb / 2, fcy, fb, fb, "A", LK_PAD, 0, 0, SWITCH_BTN_A);
        live_add(fcx - fb / 2, fcy + fb + 6, fb, fb, "B", LK_PAD, 0, 0, SWITCH_BTN_B);
        draw::text(fcx - 60, fcy + 2 * fb + 24, 22, draw::MUTED, "BUTTONS");

        // shoulders row (centered top)
        float sh_y = body_y + 40, sh_w = 150, sh_h = 72, sh_g = 16;
        const char *shl[] = {"L", "ZL", "R", "ZR"};
        uint16_t shb[] = {SWITCH_BTN_L, SWITCH_BTN_ZL, SWITCH_BTN_R, SWITCH_BTN_ZR};
        float sh_total = 4 * sh_w + 3 * sh_g;
        float sh_x = cxp - sh_total / 2;
        for (int i = 0; i < 4; i++) live_add(sh_x + i * (sh_w + sh_g), sh_y, sh_w, sh_h, shl[i], LK_PAD, 0, 0, shb[i]);

        // system row (centered middle). Home and Capture are intercepted by the
        // console and never reach the target over USB, so they are not shown.
        float sy_y = body_y + 170, sy_w = 150, sy_h = 72, sy_g = 16;
        const char *syl[] = {"-", "+", "L3", "R3"};
        uint16_t syb[] = {SWITCH_BTN_MINUS, SWITCH_BTN_PLUS, SWITCH_BTN_L3, SWITCH_BTN_R3};
        float sy_total = 4 * sy_w + 3 * sy_g;
        float sy_x = cxp - sy_total / 2;
        for (int i = 0; i < 4; i++) live_add(sy_x + i * (sy_w + sy_g), sy_y, sy_w, sy_h, syl[i], LK_PAD, 0, 0, syb[i]);

        draw::text(cxp - draw::text_size(24, "Hold a button to keep it pressed.").x / 2, body_y + 300, 24, draw::MUTED, "Hold a button to keep it pressed.");
    }

    (void)carry_x;
    for (int i = 0; i < g_live_key_count; i++) draw_live_key(g_live_keys[i], i);
    // overlay active state for carry/record toolbar buttons
    for (int i = 0; i < g_live_key_count; i++) {
        const LiveKey &k = g_live_keys[i];
        if ((k.kind == LK_CARRY && carry_enabled()) || (k.kind == LK_RECORD && record_active())) {
            draw::frame(k.r.x, k.r.y, k.r.w, k.r.h, k.kind == LK_RECORD ? draw::RED_HOT : draw::ACCENT, 16, 2.5f);
        }
        if (k.kind == LK_CARRY) {
            toggle_switch(k.r.x + 16, k.r.y + (k.r.h - 36) / 2, carry_enabled());
            ImVec2 lt = draw::text_size(27, "Pass through Joy-Con", true);
            draw::text(k.r.x + 92, k.r.y + (k.r.h - lt.y) / 2, 27, draw::TEXT, "Pass through Joy-Con", true);
        }
    }

    Hint hints[] = {{ACT_L1, ""}, {ACT_R1, "Switch tab"}};
    draw_bottom(hints, 2);
}

void draw_ldn(const C7sAppSnapshot &s, const UsbCompositeStatus *u) {
    draw_top(s, u);
    float x = MARGIN, y = CONTENT_Y, w = W - 2 * MARGIN, h = CONTENT_H;
    draw::panel(x, y, w, h);
    draw::text(x + 28, y + 22, 34, draw::TEXT, "LDN injection", true);
    draw::text(x + 28, y + 78, 24, draw::MUTED,
               "On the Switch 2: MULTIPLAYER > WIRELESS > CREATE GAME > HOST LOBBY (+).");
    draw::text(x + 28, y + 110, 24, draw::MUTED,
               "Keep the lobby open. The Switch 1 joins over local wireless. No prod.keys needed.");
    bool active = ldn_route_active();
    g_ldn_start_rect = {x + 28, y + 164, 320, 76};
    draw::accent_button(g_ldn_start_rect.x, g_ldn_start_rect.y, g_ldn_start_rect.w,
                        g_ldn_start_rect.h, active ? "LDN RUNNING" : "START LDN", true);
    draw::text(x + 28, y + 272, 26, active ? draw::ACCENT : draw::TEXT, ldn_route_status(), true);
    draw::text(x + 28, y + 318, 22, draw::MUTED, "LOG", true);
    g_ldn_log_btn = {x + 28, y + 348, 220, 56};
    draw::accent_button(g_ldn_log_btn.x, g_ldn_log_btn.y, g_ldn_log_btn.w,
                        g_ldn_log_btn.h, "FULL LOG", true);
    draw::text_clip(x + 270, y + 362, 18, draw::MUTED, ldn_route_log(), w - 300);

    if (g_ldn_log_full) {
        draw::rect(0, 0, W, H, draw::BG, 0);
        draw::text(40, 30, 40, draw::TEXT, "LDN LOG", true);
        draw::text(40, 80, 22, draw::MUTED, "Up/Down scroll - B close");
        char buf[16384];
        snprintf(buf, sizeof(buf), "%s", ldn_route_log());
        int line = 0;
        char *p = buf;
        while (p && *p && line < 44) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            if (line >= g_ldn_log_scroll) {
                draw::text_clip(40, 130 + (line - g_ldn_log_scroll) * 22, 18, draw::TEXT, p, W - 80);
            }
            line++;
            if (!nl) break;
            p = nl + 1;
        }
        Hint hints[] = {{ACT_CIRCLE, "Close"}};
        draw_bottom(hints, 1);
    } else {
        Hint hints[] = {{ACT_OPTIONS, "Hold + to exit"}};
        draw_bottom(hints, 1);
    }
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
    g_overlay = OV_RUNNING;
}

void set_tab(Tab t) {
    if (t == g_tab) return;
    if (g_tab == TAB_LIVE) leave_live();
    g_tab = t;
    if (g_tab == TAB_LIVE) enter_live();
}

void handle_input(const Input &in, const C7sAppSnapshot &s) {
    // overlays first
    if (g_overlay == OV_RUNNING) {
        if (in.b) app_request_cancel();
        if (in.touch && g_cancel_rect.hit(in.tx, in.ty)) app_request_cancel();
        return;
    }
    if (g_overlay == OV_RESULT) {
        if (in.a || (in.touch && g_again_rect.hit(in.tx, in.ty))) {
            g_overlay = OV_NONE;
            refresh_entries(false);
            start_selected();
        } else if (in.b || (in.touch && g_back_rect.hit(in.tx, in.ty))) {
            g_overlay = OV_NONE;
            refresh_entries(false);
        }
        return;
    }

    // tab switching (shoulders)
    if (in.l) set_tab((Tab)((g_tab + 3) % 4));
    if (in.r) set_tab((Tab)((g_tab + 1) % 4));
    if (in.touch) {
        for (int i = 0; i < 4; i++)
            if (g_tab_rects[i].hit(in.tx, in.ty)) set_tab((Tab)i);
    }

    switch (g_tab) {
        case TAB_SEND:
            if (g_count > 0) {
                if (in.up) g_cursor = (g_cursor + g_count - 1) % g_count, refresh_preview();
                if (in.down) g_cursor = (g_cursor + 1) % g_count, refresh_preview();
            }
            if (in.a && g_count > 0) start_selected();
            if (in.x) refresh_entries(false);
            if (in.touch) {
                for (int i = 0; i < g_count; i++)
                    if (g_row_rects[i].hit(in.tx, in.ty)) { g_cursor = i; refresh_preview(); }
                if (g_start_rect.hit(in.tx, in.ty) && g_count > 0) start_selected();
                if (g_refresh_rect.hit(in.tx, in.ty)) refresh_entries(false);
            }
            break;
        case TAB_LIVE:
            if (in.zl) g_liveview = LV_KBD;
            if (in.zr) g_liveview = LV_PAD;
            if (in.touch_down) {
                if (g_kbdtab_rect.hit(in.tx, in.ty)) g_liveview = LV_KBD;
                else if (g_padtab_rect.hit(in.tx, in.ty)) g_liveview = LV_PAD;
                else {
                    for (int i = 0; i < g_live_key_count; i++)
                        if (g_live_keys[i].r.hit(in.tx, in.ty)) { live_key_press(i); break; }
                }
            }
            if (in.touch_up) live_key_release();
            break;
        case TAB_LDN:
            if (g_ldn_log_full) {
                if (in.up) g_ldn_log_scroll = g_ldn_log_scroll > 0 ? g_ldn_log_scroll - 1 : 0;
                if (in.down) g_ldn_log_scroll++;
                if (in.b || (in.touch && g_ldn_log_btn.hit(in.tx, in.ty))) g_ldn_log_full = false;
            } else if (in.touch && g_ldn_log_btn.hit(in.tx, in.ty)) {
                g_ldn_log_full = true;
                g_ldn_log_scroll = 0;
            } else if (in.touch && !ldn_route_active() && g_ldn_start_rect.hit(in.tx, in.ty)) {
                ldn_route_start();
            }
            break;
        case TAB_CONNECT:
            if (in.y || (in.touch && g_ktest_rect.hit(in.tx, in.ty))) {
                app_request_ktest();
                set_toast("Test keys in 5s - focus a text field");
            }
            if (in.x || (in.touch && g_port_rect.hit(in.tx, in.ty))) {
                unsigned short np;
                if (ask_port(s.port, &np)) {
                    settings_save_port(np);
                    app_request_restart(np);
                    set_toast("Restarting web server on port %u...", (unsigned)np);
                }
            }
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

        if (s.running && g_overlay != OV_RUNNING) g_overlay = OV_RUNNING;
        if (g_overlay == OV_RUNNING && !s.running && s.finished) g_overlay = OV_RESULT;
        if (ticks_ms() - g_last_refresh > 2000 && g_overlay == OV_NONE) refresh_entries(false);

        Input in = poll_input();
        if (carry_enabled()) {
            in.up = in.down = in.left = in.right = false;
            in.a = in.b = in.x = in.y = false;
            in.l = in.r = in.zl = in.zr = false;
            in.minus = false;
        }
        if (in.plus_held) {
            if (g_plus_start == 0) {
                g_plus_start = ticks_ms();
            } else if (ticks_ms() - g_plus_start >= 2000) {
                g_plus_start = 0;
                if (s.running) app_request_cancel();
                g_quit = true;
            }
        } else {
            g_plus_start = 0;
        }
        handle_input(in, s);

        frame_begin();
        draw::rect(0, 0, W, H, draw::BG, 0);
        draw_top(s, u);
        switch (g_tab) {
            case TAB_SEND: draw_send(s, u); break;
            case TAB_LIVE: draw_live(s, u); break;
            case TAB_CONNECT: draw_connect(s, u); break;
            case TAB_LDN: draw_ldn(s, u); break;
        }
        if (g_overlay == OV_RUNNING) draw_running(s);
        else if (g_overlay == OV_RESULT) draw_result(s);
        draw_toast();
        frame_end();
        svcSleepThread(16000000ull);
    }

    // Graceful shutdown (chiral-ftp style): returning from main hands control
    // to libnx, which runs userAppExit and tears down the socket driver. Any
    // thread still holding a socket or USB handle at that point makes the
    // process fall over on the way out instead of returning to hbmenu. So ask
    // the web server to stop, then keep the applet pumped and the frame
    // repainting while it actually comes down -- bounded, then exit anyway.
    app_exit_trace("ui: closing");
    httpd_request_stop();
    for (uint64_t until = ticks_ms() + 4000; ticks_ms() < until; ) {
        if (!httpd_running()) break;
        if (!appletMainLoop()) break;
        frame_begin();
        draw::rect(0, 0, W, H, draw::BG, 0);
        ImVec2 cts = draw::text_size(42, "Closing...", true);
        draw::text((W - cts.x) / 2, (H - cts.y) / 2, 42, draw::TEXT, "Closing...", true);
        frame_end();
        svcSleepThread(40000000ull);
    }
    app_exit_trace("ui: httpd running=%d after wait", httpd_running());
    httpd_stop();
    app_exit_trace("ui: httpd stopped");
    // Cancels any running payload (which now releases held keys), drains the
    // USB endpoints and exits the gadget, then joins the worker thread.
    usb_worker_stop();
    app_exit_trace("ui: worker stopped");
    gfx_shutdown();
    app_exit_trace("ui: gfx shutdown");
    return 0;
}
