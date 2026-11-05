#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "usb_keyboard.h"

static PadState g_pad;
static volatile bool g_quit;

static u64 now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

static void pump(u32 ms) {
    u64 end = now_ms() + ms;
    while (now_ms() < end && !g_quit) {
        keyboard_poll(now_ms());
        padUpdate(&g_pad);
        if (!appletMainLoop()) g_quit = true;
        consoleUpdate(NULL);
        svcSleepThread(1000000ull);
    }
}

static bool wait_idle(void) {
    u64 start = now_ms();
    while (keyboard_status()->transfer_pending && !g_quit) {
        keyboard_poll(now_ms());
        if (now_ms() - start > 2000) return false;
        svcSleepThread(1000000ull);
    }
    return true;
}

static void tap(u8 key) {
    wait_idle();
    keyboard_send(0, key, now_ms());
    pump(80);
    wait_idle();
    keyboard_send(0, 0, now_ms());
    pump(80);
}

static void type_hello(void) {
    static const u8 keys[] = {0x0B, 0x08, 0x0F, 0x0F, 0x12};
    for (size_t i = 0; i < sizeof(keys); i++) tap(keys[i]);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/plume-usb", 0777);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    printf("Plume USB keyboard test (exact plume module)\n");
    consoleUpdate(NULL);

    Result rc = keyboard_init();
    if (R_FAILED(rc)) {
        printf("keyboard_init failed: 0x%x (%s)\n", rc, keyboard_status()->stage);
        consoleUpdate(NULL);
        while (!g_quit) {
            keyboard_poll(now_ms());
            padUpdate(&g_pad);
            if (!appletMainLoop()) break;
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
            svcSleepThread(16000000ull);
        }
        consoleExit(NULL);
        return 1;
    }

    u64 start = now_ms();
    while (!keyboard_status()->ready && now_ms() - start < 30000 && !g_quit) {
        keyboard_poll(now_ms());
        padUpdate(&g_pad);
        if (!appletMainLoop()) g_quit = true;
        consoleUpdate(NULL);
        svcSleepThread(1000000ull);
    }
    printf("USB: %s\n", keyboard_status()->ready ? "READY" : "NOT READY");
    printf("Focus a text field on the other console.\n");
    printf("Typing 'hello' in 5 seconds. A = type again, + = exit.\n");
    consoleUpdate(NULL);

    pump(5000);
    if (!g_quit) {
        type_hello();
        printf("sent hello\n");
        consoleUpdate(NULL);
    }

    while (!g_quit) {
        keyboard_poll(now_ms());
        padUpdate(&g_pad);
        u64 down = padGetButtonsDown(&g_pad);
        if (!appletMainLoop() || (down & HidNpadButton_Plus)) break;
        if (down & HidNpadButton_A) {
            pump(300);
            if (!g_quit) {
                type_hello();
                printf("sent hello\n");
                consoleUpdate(NULL);
            }
        }
        consoleUpdate(NULL);
        svcSleepThread(16000000ull);
    }

    keyboard_exit();
    consoleExit(NULL);
    return 0;
}
