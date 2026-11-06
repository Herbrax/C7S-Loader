#include "usb_composite.h"

#include "app.h"
#include "debuglog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DMA_ALIGN __attribute__((aligned(0x1000)))

#define EP_DIR_OUT 0x00
#define EP_DIR_IN  0x80
/* usb:ds allocates endpoint numbers as interface_index+1 per direction, so
   interface 0 (gamepad) gets IN 0x81 / OUT 0x01 and interface 1 (keyboard)
   gets IN 0x82. The keyboard is IN-only; its LEDs arrive over EP0 (SET_REPORT),
   so there is no keyboard OUT endpoint. This matches the layout the Switch 2
   accepts for a HORI-identity composite (see RecurCIV / PicoSwitch). */
#define EP_PAD_OUT 0x01
#define EP_PAD_IN  0x81
#define EP_KBD_IN  0x82

#define PAD_REPORT_LEN 8
#define KBD_REPORT_LEN 8
#define CONTROL_TIMEOUT_MS 1500
#define REPORT_TIMEOUT_MS 2500
#define READY_SETTLE_MS 300

typedef struct {
    UsbDsInterface *intf;
    UsbDsEndpoint *ep_in;
    UsbDsEndpoint *ep_out;
    uint8_t intf_num;
    bool is_keyboard;
    bool control_owner;
    uint8_t control_target;
    uint8_t ep_size;
    const uint8_t *report_desc;
    uint16_t report_desc_len;
    uint8_t hid_desc[9];
    uint8_t last_report[64];
    uint16_t last_report_len;
    bool descriptor_valid;
    bool ready;
    bool pending;
    uint32_t xfer_id;
    uint64_t xfer_started;
    uint32_t sent;
    bool out_pending;
    uint32_t out_id;
    uint64_t out_started;
    uint8_t echo[64];
    uint16_t echo_len;
    bool echo_pending;
    uint8_t *xfer_out;
    bool ctrl_in_pending;
    bool ctrl_out_pending;
    bool out_status_pending;
    bool led_pending;
    bool control_read;
    bool descriptor_request;
    bool descriptor_in_done;
    bool descriptor_out_done;
    bool status_out_after_in;
    uint32_t ctrl_in_id;
    uint32_t ctrl_out_id;
    uint32_t ctrl_in_expected;
    uint32_t ctrl_out_expected;
    uint64_t control_started;
    uint8_t leds;
    uint8_t idle;
    uint8_t protocol;
    uint8_t *xfer;
    uint8_t *ctrl_in;
    uint8_t *ctrl_out;
} c7s_itf_t;

DMA_ALIGN static uint8_t g_pad_xfer[0x1000];
DMA_ALIGN static uint8_t g_pad_cin[0x1000];
DMA_ALIGN static uint8_t g_pad_cout[0x1000];
DMA_ALIGN static uint8_t g_pad_xout[0x1000];
DMA_ALIGN static uint8_t g_kbd_xfer[0x1000];
DMA_ALIGN static uint8_t g_kbd_cin[0x1000];
DMA_ALIGN static uint8_t g_kbd_cout[0x1000];
DMA_ALIGN static uint8_t g_kbd_xout[0x1000];

static c7s_itf_t g_pad;
static c7s_itf_t g_kbd;
static UsbCompositeStatus g_status;
static UsbState g_state = UsbState_Detached;
static uint64_t g_configured_at;
static uint64_t g_now;
static uint64_t g_last_status_log;
static bool g_service_open;
static FILE *g_log;

static void usb_log(const char *fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    dbg_log_usb("%s", line);
    if (!g_log) return;
    fprintf(g_log, "%s\n", line);
    fflush(g_log);
}

static Result failed(const char *stage, Result rc) {
    g_status.error = rc;
    snprintf(g_status.stage, sizeof(g_status.stage), "%s", stage);
    usb_log("ERROR %s: 0x%08lx", stage, (unsigned long)rc);
    return rc;
}

static bool signalled(Event *event) {
    if (R_FAILED(eventWait(event, 0))) return false;
    eventClear(event);
    return true;
}

static void make_hid_desc(uint8_t out[9], uint16_t report_len) {
    out[0] = 9;
    out[1] = USB_DT_HID;
    out[2] = 0x11;
    out[3] = 0x01;
    out[4] = 0;
    out[5] = 1;
    out[6] = USB_DT_REPORT;
    out[7] = (uint8_t)(report_len & 0xFF);
    out[8] = (uint8_t)(report_len >> 8);
}

static void it_stall(c7s_itf_t *it) {
    usbDsInterface_StallCtrl(it->intf);
    it->ctrl_in_pending = false;
    it->ctrl_out_pending = false;
    it->out_status_pending = false;
    it->led_pending = false;
    it->status_out_after_in = false;
    it->descriptor_request = false;
}

static void it_queue_status_out(c7s_itf_t *it) {
    Result rc = usbDsInterface_CtrlOutPostBufferAsync(it->intf, it->ctrl_out, 0, &it->ctrl_out_id);
    if (R_FAILED(rc)) {
        failed("USB control OUT status", rc);
        it_stall(it);
        return;
    }
    it->ctrl_out_pending = true;
    it->ctrl_out_expected = 0;
    it->control_started = g_now;
}

static void it_reply(c7s_itf_t *it, const void *data, size_t size, uint16_t requested) {
    if (size > requested) size = requested;
    if (size) memcpy(it->ctrl_in, data, size);
    it->status_out_after_in = it->control_read;
    Result rc = usbDsInterface_CtrlInPostBufferAsync(it->intf, it->ctrl_in, size, &it->ctrl_in_id);
    if (R_FAILED(rc)) {
        failed("USB control IN response", rc);
        it_stall(it);
        return;
    }
    it->ctrl_in_pending = true;
    it->ctrl_in_expected = (uint32_t)size;
    it->control_started = g_now;
}

static void it_handle_setup(c7s_itf_t *it) {
    struct usb_control_setup s = {0};
    Result rc = usbDsInterface_GetSetupPacket(it->intf, &s, sizeof(s));
    if (R_FAILED(rc)) {
        failed("Read USB setup request", rc);
        return;
    }
    usb_log("SETUP itf=%u type=%02x req=%02x value=%04x index=%04x length=%u",
            it->intf_num, s.bmRequestType, s.bRequest, s.wValue, s.wIndex, s.wLength);

    it->ctrl_in_pending = false;
    it->ctrl_out_pending = false;
    it->out_status_pending = false;
    it->led_pending = false;
    it->status_out_after_in = false;
    it->control_read = (s.bmRequestType & 0x80) != 0;
    it->descriptor_request = false;
    it->control_target = (s.wIndex == 0) ? 0 : 1;
    c7s_itf_t *logical = (s.wIndex == 0) ? &g_pad : &g_kbd;

    uint8_t type = s.bmRequestType;
    if (type == 0x81 && s.bRequest == 0 && s.wValue == 0) {
        const uint16_t flags = 0;
        it_reply(it, &flags, 2, s.wLength);
    } else if (type == 0x81 && s.bRequest == 10 && s.wValue == 0) {
        const uint8_t alt = 0;
        it_reply(it, &alt, 1, s.wLength);
    } else if (type == 0x01 && s.bRequest == 11 && s.wValue == 0 && s.wLength == 0) {
        it_reply(it, NULL, 0, 0);
    } else if (type == 0x81 && s.bRequest == 6 && (s.wValue >> 8) == USB_DT_REPORT && (s.wValue & 0xFF) == 0) {
        it->descriptor_request = s.wLength >= logical->report_desc_len;
        if (it->descriptor_request) {
            it->descriptor_in_done = false;
            it->descriptor_out_done = false;
        }
        it_reply(it, logical->report_desc, logical->report_desc_len, s.wLength);
    } else if (type == 0x81 && s.bRequest == 6 && (s.wValue >> 8) == USB_DT_HID && (s.wValue & 0xFF) == 0) {
        it_reply(it, logical->hid_desc, sizeof(logical->hid_desc), s.wLength);
    } else if (type == 0xA1 && s.bRequest == 1 && (s.wValue & 0xFF) == 0) {
        if ((s.wValue >> 8) == 1) {
            if (logical->is_keyboard) {
                const uint8_t zero[8] = {0};
                it_reply(it, zero, sizeof(zero), s.wLength);
            } else {
                const C7sPadReport neutral = {
                    .buttons = 0, .hat = SWITCH_HAT_NOTHING,
                    .lx = SWITCH_STICK_MID, .ly = SWITCH_STICK_MID,
                    .rx = SWITCH_STICK_MID, .ry = SWITCH_STICK_MID, .vendor = 0};
                it_reply(it, &neutral, sizeof(neutral), s.wLength);
            }
        } else if ((s.wValue >> 8) == 2) {
            it_reply(it, &logical->leds, 1, s.wLength);
        } else {
            it_stall(it);
        }
    } else if (type == 0xA1 && s.bRequest == 2) {
        it_reply(it, &logical->idle, 1, s.wLength);
    } else if (type == 0xA1 && s.bRequest == 3) {
        it_reply(it, &logical->protocol, 1, s.wLength);
    } else if (type == 0x21 && s.bRequest == 10 && s.wLength == 0 && (s.wValue & 0xFF) == 0) {
        logical->idle = (uint8_t)(s.wValue >> 8);
        it_reply(it, NULL, 0, 0);
    } else if (type == 0x21 && s.bRequest == 11 && s.wLength == 0 && s.wValue <= 1) {
        logical->protocol = (uint8_t)s.wValue;
        it_reply(it, NULL, 0, 0);
    } else if (type == 0x21 && s.bRequest == 9 && (s.wValue & 0xFF) == 0 && s.wLength <= 64) {
        memset(it->ctrl_out, 0, s.wLength);
        rc = usbDsInterface_CtrlOutPostBufferAsync(it->intf, it->ctrl_out, s.wLength, &it->ctrl_out_id);
        if (R_FAILED(rc)) {
            failed("Receive USB output report", rc);
            it_stall(it);
            return;
        }
        it->ctrl_out_pending = true;
        it->ctrl_out_expected = s.wLength;
        it->out_status_pending = true;
        it->led_pending = logical->is_keyboard && s.wLength >= 1;
        it->control_started = g_now;
    } else {
        it_stall(it);
    }
}

static void it_reset(c7s_itf_t *it) {
    if (it->pending && it->ep_in) {
        usbDsEndpoint_Cancel(it->ep_in);
        it->pending = false;
        UsbDsReportData reports;
        eventClear(&it->ep_in->CompletionEvent);
        usbDsEndpoint_GetReportData(it->ep_in, &reports);
    }
    it->ctrl_in_pending = false;
    it->ctrl_out_pending = false;
    it->out_status_pending = false;
    it->led_pending = false;
    it->status_out_after_in = false;
    it->descriptor_request = false;
    it->descriptor_in_done = false;
    it->descriptor_out_done = false;
    if (it->ep_out && it->out_pending) {
        usbDsEndpoint_Cancel(it->ep_out);
        it->out_pending = false;
        UsbDsReportData reports;
        eventClear(&it->ep_out->CompletionEvent);
        usbDsEndpoint_GetReportData(it->ep_out, &reports);
    }
    it->echo_pending = false;
    it->echo_len = 0;
    it->descriptor_valid = false;
    it->ready = false;
    memset(it->last_report, 0, sizeof(it->last_report));
    it->last_report_len = 0;
    it->leds = 0;
    it->idle = 0;
    it->protocol = 1;
}

static Result it_send(c7s_itf_t *it, const void *data, size_t len);

static bool it_ready(c7s_itf_t *it) {
    (void)it;
    return g_state == UsbState_Configured && g_now >= g_configured_at &&
           g_now - g_configured_at >= READY_SETTLE_MS;
}

static Result it_post_out(c7s_itf_t *it) {
    if (!it->ep_out || it->out_pending) return 0;
    size_t len = it->is_keyboard ? 8 : 64;
    memset(it->xfer_out, 0, len);
    armDCacheFlush(it->xfer_out, len);
    Result rc = usbDsEndpoint_PostBufferAsync(it->ep_out, it->xfer_out, len, &it->out_id);
    if (R_FAILED(rc)) return rc;
    it->out_pending = true;
    it->out_started = g_now;
    return 0;
}

static void it_poll(c7s_itf_t *it) {
    if (it->control_owner) {
    if (signalled(&it->intf->CtrlInCompletionEvent)) {
        UsbDsReportData reports;
        Result rc = usbDsInterface_GetCtrlInReportData(it->intf, &reports);
        if (R_SUCCEEDED(rc)) {
            bool found = false;
            for (uint32_t i = 0; i < reports.report_count && i < 8; i++) {
                if (it->ctrl_in_pending && reports.report[i].id == it->ctrl_in_id) found = true;
            }
            if (found) {
                uint32_t actual = 0;
                rc = usbDsParseReportData(&reports, it->ctrl_in_id, NULL, &actual);
                it->ctrl_in_pending = false;
                if (R_SUCCEEDED(rc) && actual == it->ctrl_in_expected) {
                    if (it->descriptor_request) it->descriptor_in_done = true;
                    if (it->status_out_after_in) {
                        it->status_out_after_in = false;
                        it_queue_status_out(it);
                    }
                } else {
                    failed("USB control IN completion", R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite));
                    it_stall(it);
                }
            }
        } else {
            failed("Read USB control IN result", rc);
        }
    }

    if (signalled(&it->intf->CtrlOutCompletionEvent)) {
        UsbDsReportData reports;
        uint32_t transferred = 0;
        Result rc = usbDsInterface_GetCtrlOutReportData(it->intf, &reports);
        if (R_SUCCEEDED(rc)) {
            bool found = false;
            for (uint32_t i = 0; i < reports.report_count && i < 8; i++) {
                if (it->ctrl_out_pending && reports.report[i].id == it->ctrl_out_id) found = true;
            }
            if (found) {
                rc = usbDsParseReportData(&reports, it->ctrl_out_id, NULL, &transferred);
                it->ctrl_out_pending = false;
                if (R_SUCCEEDED(rc) && transferred == it->ctrl_out_expected) {
                    if (it->out_status_pending) {
                        c7s_itf_t *logical = (it->control_target == 0) ? &g_pad : &g_kbd;
                        if (it->led_pending && it->ctrl_out_expected >= 1) {
                            armDCacheFlush(it->ctrl_out, it->ctrl_out_expected);
                            logical->leds = it->ctrl_out[0];
                            g_status.leds = logical->leds;
                            g_status.leds_known = true;
                        } else if (it->control_target == 0 && it->ctrl_out_expected > 0 &&
                                   it->ctrl_out_expected <= sizeof(logical->echo)) {
                            armDCacheFlush(it->ctrl_out, it->ctrl_out_expected);
                            memcpy(logical->echo, it->ctrl_out, it->ctrl_out_expected);
                            logical->echo_len = (uint16_t)it->ctrl_out_expected;
                            logical->echo_pending = true;
                            usb_log("itf=%u control output queued echo len=%u", logical->intf_num,
                                    (unsigned)logical->echo_len);
                        }
                        it->out_status_pending = false;
                        it->led_pending = false;
                        it_reply(it, NULL, 0, 0);
                    } else if (it->descriptor_request) {
                        it->descriptor_out_done = true;
                    }
                } else {
                    failed("USB control OUT completion", R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite));
                    it_stall(it);
                }
            }
        } else {
            failed("Read USB control OUT result", rc);
        }
    }

    if (it->descriptor_request && it->descriptor_in_done && it->descriptor_out_done) {
        c7s_itf_t *logical = (it->control_target == 0) ? &g_pad : &g_kbd;
        if (!logical->descriptor_valid) {
            logical->descriptor_valid = true;
            usb_log("itf=%u report descriptor acknowledged", logical->intf_num);
        }
    }

    if (signalled(&it->intf->SetupEvent)) it_handle_setup(it);

    if ((it->ctrl_in_pending || it->ctrl_out_pending) && g_now - it->control_started > CONTROL_TIMEOUT_MS) {
        usb_log("itf=%u control timeout", it->intf_num);
        it_stall(it);
    }
    }

    if (it->ep_in && signalled(&it->ep_in->CompletionEvent)) {
        UsbDsReportData reports;
        uint32_t transferred = 0;
        Result rc = usbDsEndpoint_GetReportData(it->ep_in, &reports);
        if (R_SUCCEEDED(rc) && it->pending) {
            rc = usbDsParseReportData(&reports, it->xfer_id, NULL, &transferred);
        }
        if (it->pending) {
            it->pending = false;
            if (R_FAILED(rc) || transferred != it->last_report_len) {
                g_status.reports_failed++;
                failed("HID report completion", R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite));
            } else {
                if (it->sent == 0) usb_log("itf=%u first report completed", it->intf_num);
                it->sent++;
                if (it->is_keyboard) g_status.kbd_sent = it->sent;
                else g_status.pad_sent = it->sent;
            }
        }
    }

    if (it->ep_out && signalled(&it->ep_out->CompletionEvent)) {
        UsbDsReportData reports;
        uint32_t transferred = 0;
        Result rc = usbDsEndpoint_GetReportData(it->ep_out, &reports);
        if (R_SUCCEEDED(rc) && it->out_pending) {
            rc = usbDsParseReportData(&reports, it->out_id, NULL, &transferred);
        }
        if (it->out_pending) {
            it->out_pending = false;
            if (R_SUCCEEDED(rc)) {
                armDCacheFlush(it->xfer_out, it->is_keyboard ? 8 : 64);
                if (it->is_keyboard) {
                    if (transferred >= 1) {
                        it->leds = it->xfer_out[0];
                        g_status.leds = it->leds;
                        g_status.leds_known = true;
                    }
                } else if (transferred > 0 && transferred <= sizeof(it->echo)) {
                    memcpy(it->echo, it->xfer_out, transferred);
                    it->echo_len = (uint16_t)transferred;
                    it->echo_pending = true;
                    usb_log("itf=%u out report queued echo len=%u", it->intf_num,
                            (unsigned)it->echo_len);
                }
            }
        }
    }

    if (g_state == UsbState_Configured && it->ep_out && !it->out_pending) it_post_out(it);

    if (it->out_pending && g_now - it->out_started > REPORT_TIMEOUT_MS) {
        usbDsEndpoint_Cancel(it->ep_out);
        it->out_pending = false;
        UsbDsReportData reports;
        eventClear(&it->ep_out->CompletionEvent);
        usbDsEndpoint_GetReportData(it->ep_out, &reports);
    }

    if (it->pending && g_now - it->xfer_started > REPORT_TIMEOUT_MS) {
        usb_log("itf=%u report timeout descriptor=%u state=%u", it->intf_num,
                it->descriptor_valid ? 1 : 0, (unsigned)g_state);
        usbDsEndpoint_Cancel(it->ep_in);
        it->pending = false;
        UsbDsReportData reports;
        eventClear(&it->ep_in->CompletionEvent);
        usbDsEndpoint_GetReportData(it->ep_in, &reports);
        g_status.reports_failed++;
        usb_log("itf=%u report canceled (host did not poll)", it->intf_num);
    }

    bool ready = it_ready(it);
    if (ready != it->ready) {
        it->ready = ready;
        usb_log("itf=%u ready=%u descriptor=%u", it->intf_num, ready ? 1 : 0, it->descriptor_valid ? 1 : 0);
    }

    if (it->echo_pending && !it->pending && it->ready) {
        if (R_SUCCEEDED(it_send(it, it->echo, it->echo_len))) it->echo_pending = false;
    }
}

static Result append_interface(UsbDsInterface *intf, uint8_t num, uint8_t subclass, uint8_t proto,
                               const uint8_t hid[9], uint8_t out_addr, uint8_t in_addr,
                               uint16_t ep_size, bool in_first, bool has_out) {
    struct usb_interface_descriptor idesc = {
        .bLength = 9,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = num,
        .bAlternateSetting = 0,
        .bNumEndpoints = (uint8_t)(has_out ? 2 : 1),
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = subclass,
        .bInterfaceProtocol = proto,
        .iInterface = 0,
    };
    struct usb_endpoint_descriptor eo = {
        .bLength = 7,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = out_addr,
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = ep_size,
        .bInterval = 1,
    };
    struct usb_endpoint_descriptor ei = {
        .bLength = 7,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = in_addr,
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = ep_size,
        .bInterval = 1,
    };
    /* SuperSpeed endpoint companion (bLength=6, bDescriptorType=0x30/48).
       bMaxBurst=0, bmAttributes=0, wBytesPerInterval = max packet size. The
       Switch 2 enumerates at SuperSpeed and will not bring the composite up
       without a companion on every endpoint. */
    const uint8_t companion[6] = {6, 48, 0, 0, (uint8_t)(ep_size & 0xFF), (uint8_t)(ep_size >> 8)};
    const UsbDeviceSpeed speeds[3] = {UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super};
    for (unsigned i = 0; i < 3; i++) {
        UsbDeviceSpeed speed = speeds[i];
        bool super = (speed == UsbDeviceSpeed_Super);
        /* 1 ms polling at every speed: bInterval 1 frame at FS, 2^(4-1) microframes at HS/SS. */
        eo.bInterval = ei.bInterval = (i == 0) ? 1 : 4;
        Result rc;
        rc = usbDsInterface_AppendConfigurationData(intf, speed, &idesc, sizeof(idesc));
        if (R_FAILED(rc)) return rc;
        rc = usbDsInterface_AppendConfigurationData(intf, speed, hid, 9);
        if (R_FAILED(rc)) return rc;
        if (!has_out) {
            rc = usbDsInterface_AppendConfigurationData(intf, speed, &ei, sizeof(ei));
            if (R_FAILED(rc)) return rc;
            if (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(intf, speed, companion, sizeof(companion)))) return rc;
        } else if (in_first) {
            rc = usbDsInterface_AppendConfigurationData(intf, speed, &ei, sizeof(ei));
            if (R_FAILED(rc)) return rc;
            if (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(intf, speed, companion, sizeof(companion)))) return rc;
            rc = usbDsInterface_AppendConfigurationData(intf, speed, &eo, sizeof(eo));
            if (R_FAILED(rc)) return rc;
            if (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(intf, speed, companion, sizeof(companion)))) return rc;
        } else {
            rc = usbDsInterface_AppendConfigurationData(intf, speed, &eo, sizeof(eo));
            if (R_FAILED(rc)) return rc;
            if (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(intf, speed, companion, sizeof(companion)))) return rc;
            rc = usbDsInterface_AppendConfigurationData(intf, speed, &ei, sizeof(ei));
            if (R_FAILED(rc)) return rc;
            if (super && R_FAILED(rc = usbDsInterface_AppendConfigurationData(intf, speed, companion, sizeof(companion)))) return rc;
        }
    }
    return 0;
}

Result usb_composite_init(void) {
    memset(&g_status, 0, sizeof(g_status));
    memset(&g_pad, 0, sizeof(g_pad));
    memset(&g_kbd, 0, sizeof(g_kbd));
    g_state = UsbState_Detached;
    g_configured_at = 0;

    rename("sdmc:/switch/C7S-Loader/usb.log", "sdmc:/switch/C7S-Loader/usb-prev.log");
    g_log = fopen("sdmc:/switch/C7S-Loader/usb.log", "w");
    usb_log("C7S-Loader composite USB | HOS %u.%u.%u | applet=%d",
            HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()),
            appletGetAppletType());

    if (hosversionBefore(5, 0, 0)) {
        return failed("Horizon OS 5.0.0 required", MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer));
    }

    Result rc = 0;
    g_pad.is_keyboard = false;
    g_pad.control_owner = true;
    g_pad.control_target = 0;
    g_pad.intf_num = 0;
    g_pad.ep_size = 64;
    g_pad.report_desc = c7s_gamepad_report_descriptor;
    g_pad.report_desc_len = C7S_GAMEPAD_REPORT_DESC_LEN;
    g_pad.xfer = g_pad_xfer;
    g_pad.ctrl_in = g_pad_cin;
    g_pad.ctrl_out = g_pad_cout;
    g_pad.xfer_out = g_pad_xout;
    g_pad.last_report_len = PAD_REPORT_LEN;
    g_pad.protocol = 1;

    g_kbd.is_keyboard = true;
    g_kbd.control_owner = false;
    g_kbd.control_target = 1;
    g_kbd.intf_num = 1;
    g_kbd.ep_size = 8;
    g_kbd.report_desc = c7s_keyboard_report_descriptor;
    g_kbd.report_desc_len = C7S_KEYBOARD_REPORT_DESC_LEN;
    g_kbd.xfer = g_kbd_xfer;
    g_kbd.ctrl_in = g_kbd_cin;
    g_kbd.ctrl_out = g_kbd_cout;
    g_kbd.xfer_out = g_kbd_xout;
    g_kbd.last_report_len = KBD_REPORT_LEN;
    g_kbd.protocol = 1;

    make_hid_desc(g_pad.hid_desc, C7S_GAMEPAD_REPORT_DESC_LEN);
    make_hid_desc(g_kbd.hid_desc, C7S_KEYBOARD_REPORT_DESC_LEN);

#define STEP(name, call) do { rc = (call); if (R_FAILED(rc)) return failed(name, rc); usb_log("ok: %s", name); } while (0)

    STEP("Open usb:ds", usbDsInitialize());
    g_service_open = true;
    STEP("Clear USB descriptors", usbDsClearDeviceData());

    uint16_t langs[] = {0x0409};
    uint8_t language, manufacturer, product;
    STEP("USB language string", usbDsAddUsbLanguageStringDescriptor(&language, langs, 1));
    STEP("USB manufacturer string", usbDsAddUsbStringDescriptor(&manufacturer, "PICO SWITCH PRO."));
    STEP("USB product string", usbDsAddUsbStringDescriptor(&product, "J24 CONTROLLER"));

    struct usb_device_descriptor dev = {
        .bLength = 18,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x0F0D,
        .idProduct = 0x0092,
        .bcdDevice = 0x0100,
        .iManufacturer = manufacturer,
        .iProduct = product,
        .iSerialNumber = 0,
        .bNumConfigurations = 1,
    };
    STEP("Full Speed descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev));
    STEP("High Speed descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev));
    /* SuperSpeed descriptor (bMaxPacketSize0 is the exponent: 2^9 = 512) plus a
       BOS descriptor advertising the USB 2.0 + SuperSpeed device capabilities.
       The Switch 2 port runs at SuperSpeed and needs both, or the composite
       device never configures. */
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 9;
    STEP("Super Speed descriptor", usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev));
    static const uint8_t bos[] = {5, 15, 22, 0, 2, 7, 16, 2, 2, 0, 0, 0, 10, 16, 3, 0, 14, 0, 1, 10, 0xff, 7};
    STEP("BOS descriptor", usbDsSetBinaryObjectStore(bos, sizeof(bos)));

    /* Register both interfaces first, append all configuration data, then
       register endpoints in the order gamepad-IN, gamepad-OUT, keyboard-IN so
       usb:ds hands out the addresses the descriptors above declare. */
    STEP("Gamepad interface 0", usbDsRegisterInterfaceEx(&g_pad.intf, 0));
    STEP("Keyboard interface 1", usbDsRegisterInterfaceEx(&g_kbd.intf, 1));

    STEP("Gamepad configuration", append_interface(g_pad.intf, 0, 0, 0, g_pad.hid_desc,
                                                   EP_PAD_OUT, EP_PAD_IN, 64, false, true));
    STEP("Keyboard configuration", append_interface(g_kbd.intf, 1, 1, 1, g_kbd.hid_desc,
                                                    0, EP_KBD_IN, 8, false, false));

    STEP("Gamepad IN endpoint", usbDsInterface_RegisterEndpoint(g_pad.intf, &g_pad.ep_in, EP_PAD_IN));
    STEP("Gamepad OUT endpoint", usbDsInterface_RegisterEndpoint(g_pad.intf, &g_pad.ep_out, EP_PAD_OUT));
    STEP("Keyboard IN endpoint", usbDsInterface_RegisterEndpoint(g_kbd.intf, &g_kbd.ep_in, EP_KBD_IN));

    STEP("Enable keyboard interface", usbDsInterface_EnableInterface(g_kbd.intf));
    STEP("Enable gamepad interface", usbDsInterface_EnableInterface(g_pad.intf));
    STEP("Enable USB device", usbDsEnable());

#undef STEP

    g_status.initialized = true;
    snprintf(g_status.stage, sizeof(g_status.stage), "USB composite initialized");
    usb_log("USB composite initialized");
    return 0;
}

void usb_composite_poll(uint64_t now_ms) {
    g_now = now_ms;
    if (!g_status.initialized) return;

    UsbState state;
    Result rc = usbDsGetState(&state);
    if (R_FAILED(rc)) {
        failed("Read USB state", rc);
        g_status.pad_ready = false;
        g_status.kbd_ready = false;
        return;
    }

    if (state != g_state) {
        usb_log("USB state %u -> %u", g_state, state);
        if (g_state == UsbState_Configured && state == UsbState_Default) g_status.bus_resets++;
        g_state = state;
        if (state == UsbState_Configured) g_configured_at = now_ms;
        if (state != UsbState_Configured) {
            it_reset(&g_pad);
            it_reset(&g_kbd);
        }
    }

    g_status.state = state;
    g_status.configured = (state == UsbState_Configured);

    it_poll(&g_pad);
    it_poll(&g_kbd);

    g_status.pad_ready = g_pad.ready;
    g_status.kbd_ready = g_kbd.ready;
    g_status.pad_pending = g_pad.pending;
    g_status.kbd_pending = g_kbd.pending;

    if (g_status.initialized && g_now - g_last_status_log >= 2000) {
        g_last_status_log = g_now;
        usb_log("state=%u cfg=%d pad=%d/%u kbd=%d/%u fail=%u pend=%d/%d",
                (unsigned)g_state, g_status.configured ? 1 : 0,
                g_pad.ready ? 1 : 0, (unsigned)g_pad.sent,
                g_kbd.ready ? 1 : 0, (unsigned)g_kbd.sent,
                (unsigned)g_status.reports_failed,
                g_pad.pending ? 1 : 0, g_kbd.pending ? 1 : 0);
    }
}

static Result it_send(c7s_itf_t *it, const void *data, size_t len) {
    if (!it->ready) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (it->pending) return 0;
    if (len > it->last_report_len) len = it->last_report_len;
    memcpy(it->xfer, data, len);
    if (len < it->last_report_len) memset(it->xfer + len, 0, it->last_report_len - len);
    armDCacheFlush(it->xfer, it->last_report_len);
    Result rc = usbDsEndpoint_PostBufferAsync(it->ep_in, it->xfer, it->last_report_len, &it->xfer_id);
    if (R_FAILED(rc)) {
        g_status.reports_failed++;
        return failed("Send HID report", rc);
    }
    it->pending = true;
    it->xfer_started = g_now;
    return 0;
}

Result usb_send_keyboard(uint8_t modifier, const uint8_t keycode[6], uint64_t now_ms) {
    g_now = now_ms;
    C7sKbdReport report;
    report.modifier = modifier;
    report.reserved = 0;
    memcpy(report.keycode, keycode, 6);
    return it_send(&g_kbd, &report, sizeof(report));
}

Result usb_send_keys(uint8_t modifier, uint8_t key1, uint64_t now_ms) {
    uint8_t keys[6] = {key1, 0, 0, 0, 0, 0};
    return usb_send_keyboard(modifier, keys, now_ms);
}

Result usb_send_gamepad(const C7sPadReport *report, uint64_t now_ms) {
    g_now = now_ms;
    return it_send(&g_pad, report, sizeof(*report));
}

bool usb_keyboard_ready(void) { return g_kbd.ready; }
bool usb_gamepad_ready(void) { return g_pad.ready; }
bool usb_keyboard_pending(void) { return g_kbd.pending; }
bool usb_gamepad_pending(void) { return g_pad.pending; }

const UsbCompositeStatus *usb_composite_status(void) { return &g_status; }

const char *usb_backend_name(void) { return "composite-hori-pico-layout"; }

void usb_composite_exit(void) {
    usb_log("USB exit | pad_sent=%lu kbd_sent=%lu failed=%lu",
            (unsigned long)g_pad.sent, (unsigned long)g_kbd.sent,
            (unsigned long)g_status.reports_failed);
    usb_log("USB exit: cancel pending transfers");
    app_exit_trace("usb: cancel");
    it_reset(&g_pad);
    it_reset(&g_kbd);
    usb_log("USB exit: usbDsExit");
    app_exit_trace("usb: usbDsExit");
    if (g_service_open) {
        usbDsExit();
        g_service_open = false;
    }
    usb_log("USB exit: done");
    app_exit_trace("usb: done");
    g_pad.intf = NULL;
    g_kbd.intf = NULL;
    g_pad.ep_in = NULL;
    g_kbd.ep_in = NULL;
    g_status.initialized = false;
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}
