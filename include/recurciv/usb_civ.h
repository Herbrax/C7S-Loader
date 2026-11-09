#pragma once
#include "usb_gamepad.h"

/* Single-main-thread two-interface lease. No descriptors or normal-mode timing
   are changed. Cancel/drain uses bounded usb:ds IPC, never waits for an event. */
typedef enum { USB_CIV_OK, USB_CIV_FAILED, USB_CIV_INTERRUPTED } UsbCivResult;
typedef struct {
    uint64_t ticket;
    unsigned completed; /* keyboard=1, gamepad=2; actual successful IN URBs only */
    UsbCivResult result;
    bool fence_blocked;
} UsbCivTransfer;
bool usb_civ_acquire(void);
bool usb_civ_owned(void);
bool usb_civ_ready(void);
bool usb_civ_idle(void);
bool usb_civ_submit(uint64_t ticket,uint8_t modifier,uint8_t key,
                    const SwitchGamepadReport *gamepad,uint64_t now_ms);
UsbCivTransfer usb_civ_transfer(void);
void usb_civ_abort(uint64_t ticket);
/* Refuses release without both completed neutral reports, or after interruption. */
bool usb_civ_release(void);
/* Forced application shutdown ONLY: close usb:ds, report unresolved cleanup,
   never claim neutral delivery. Not a way to resume ordinary writers. */
void usb_civ_shutdown(void);
