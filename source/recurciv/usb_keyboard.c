#include "usb_keyboard.h"
#include "usb_gamepad.h"
#include "usb_civ.h"
#include "switch_gamepad.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* A single boot keyboard, no report IDs, six-key rollover, LED output on EP0.
   This descriptor is unchanged from the working Plume keyboard build. */
static const uint8_t report_descriptor[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,
    0x75,0x08,0x81,0x03,0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,
    0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x03,0x95,0x06,
    0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,
    0x65,0x81,0x00,0xc0
};
static struct usb_hid_descriptor gamepad_hid_descriptor = {
    .bLength=9,.bDescriptorType=USB_DT_HID,.bcdHID=0x0111,.bCountryCode=0,
    .bNumDescriptors=1,.bClassDescriptorType=USB_DT_REPORT,
    .wClassDescriptorLength=sizeof(switch_gamepad_report_descriptor)
};
static struct usb_hid_descriptor hid_descriptor = {
    .bLength=9,.bDescriptorType=USB_DT_HID,.bcdHID=0x0111,.bCountryCode=0,
    .bNumDescriptors=1,.bClassDescriptorType=USB_DT_REPORT,
    .wClassDescriptorLength=sizeof(report_descriptor)
};
_Static_assert(sizeof(struct usb_hid_descriptor)==9,"HID descriptor must be packed");
_Static_assert(sizeof(struct usb_control_setup)==8,"USB SETUP packet must be 8 bytes");
static KeyboardStatus status;
static UsbDsInterface *intf;              /* keyboard interface 1 */
static UsbDsInterface *control_intf;      /* interface that owns the current control request */
static UsbDsInterface *ctrl_in_intf;
static UsbDsInterface *ctrl_out_intf;
static UsbDsEndpoint *endpoint;           /* keyboard IN 0x82 */
static UsbDsInterface *gamepad_intf;      /* gamepad interface 0 */
static UsbDsEndpoint *gamepad_endpoint_in;   /* 0x81 */
static UsbDsEndpoint *gamepad_endpoint_out;  /* 0x01 */
static bool service_open;
#ifdef _MSC_VER
#define DMA_ALIGN __declspec(align(4096))
#else
#define DMA_ALIGN __attribute__((aligned(0x1000)))
#endif
DMA_ALIGN static uint8_t transfer_buffer[0x1000];
DMA_ALIGN static uint8_t ctrl_in[0x1000];
DMA_ALIGN static uint8_t ctrl_out[0x1000];
static uint8_t last_report[8];
DMA_ALIGN static uint8_t gamepad_transfer_buffer[0x1000];
DMA_ALIGN static uint8_t gamepad_out_buffer[0x1000];
static SwitchGamepadReport gamepad_last_report;
static SwitchGamepadReport gamepad_desired_report;
static uint32_t gamepad_transfer_id, gamepad_out_id;
static uint64_t gamepad_transfer_started;
static bool gamepad_transfer_pending, gamepad_out_pending;
static bool gamepad_error_logged;
static GamepadExclusiveState gamepad_exclusive;
/* The phase associated with the actual in-flight DMA buffer, not desired state. */
static GamepadExclusiveState gamepad_exclusive_transfer;
/* Fence OUT requests spanning either side of an exclusive session. */
static uint64_t gamepad_owner_generation, gamepad_out_generation, gamepad_ctrl_generation;
/* Diagnostic sampling only; never controls USB transfers. */
static bool diag_readiness_seen, diag_configured_ready, diag_keyboard_desc;
static bool diag_gamepad_desc, diag_gamepad_ready;
static bool diag_submit_logged, diag_completion_logged, diag_failure_logged;
static Result diag_failure_rc;
static uint32_t transfer_id, ctrl_in_id, ctrl_out_id;
static uint64_t transfer_started, last_send, control_started;
static bool ctrl_in_pending, ctrl_out_pending, led_data_pending, gamepad_data_pending;
static bool control_read, descriptor_request, descriptor_in_done, descriptor_out_done;
static UsbDsInterface *descriptor_intf;
static bool gamepad_descriptor_valid, usb_configured_ready;
static bool status_out_after_in;
static uint32_t ctrl_in_expected, ctrl_out_expected;
static uint64_t configured_at, log_milliseconds, log_origin;
static bool log_origin_set;
static FILE *log_file;
static KeyboardLogError log_error;
static bool civ_owned,civ_aborted;
/* A failed post may not supply a usable URB ID. Never let an old completion
   with the previous ID prove that such a buffer is safe to reuse. */
static unsigned civ_unknown_submission;
static UsbCivTransfer civ_transfer;
static void civ_poll_input(UsbDsEndpoint *ep,uint32_t id,bool *pending,
                           uint64_t started,uint64_t now,unsigned bit);
static void civ_interrupted(void) {
    if(civ_owned) civ_transfer.result=USB_CIV_INTERRUPTED;
}

static void remember_log_error(const char *operation,int error_number) {
    if(!log_error.failed) log_error=(KeyboardLogError){true,error_number,operation};
}

const KeyboardLogError *keyboard_log_error(void) { return &log_error; }

void keyboard_log(const char *format, ...) {
    if (!log_file) return;
    int saved_errno=errno;
    errno=0;
    if(fprintf(log_file,"[%8llu ms] ",(unsigned long long)log_milliseconds)<0)
        remember_log_error("fprintf",errno);
    va_list args; va_start(args,format);
    errno=0;
    int written=vfprintf(log_file,format,args), write_errno=errno;
    va_end(args);
    if(written<0) remember_log_error("vfprintf",write_errno);
    errno=0;
    if(fputc('\n',log_file)==EOF) remember_log_error("fputc",errno);
    errno=0;
    if(fflush(log_file)==EOF) remember_log_error("fflush",errno);
    errno=saved_errno;
}

bool keyboard_create_log_snapshot(uint64_t now_ms,KeyboardLogError *result) {
    if(!result) return false;
    *result=(KeyboardLogError){0};
    if(log_file) {
        errno=0;
        if(fflush(log_file)==EOF) *result=(KeyboardLogError){true,errno,"fflush"};
        errno=0;
        if(fclose(log_file)==EOF && !result->failed) *result=(KeyboardLogError){true,errno,"fclose"};
        log_file=NULL;
        if(result->failed) return false;
    }
    errno=0;
    log_file=fopen("sdmc:/switch/C7S-Loader/usb.log","a");
    if(!log_file) { *result=(KeyboardLogError){true,errno,"fopen"};return false; }
    errno=0;
    int written=fprintf(log_file,
        "\n--- USB diagnostic snapshot ---\n"
        "now_ms=%llu state=%u configured_ready=%u keyboard_desc=%u gamepad_desc=%u gamepad_ready=%u resets=%lu\n"
        "keyboard initialized=%u ready=%u pending=%u completed=%lu failed=%lu ctrl_in=%lu ctrl_out=%lu error=%08lx stage=%s\n"
        "gamepad EP81/01 available=%u pending=%u urb=%lu pending_age_ms=%llu keyboard EP82\n"
        "--- end snapshot ---\n",
        (unsigned long long)now_ms,(unsigned)status.state,usb_configured_ready,
        status.descriptor_valid,gamepad_descriptor_valid,gamepad_ready(),(unsigned long)status.bus_resets,
        status.initialized,status.ready,status.transfer_pending,(unsigned long)status.reports_completed,
        (unsigned long)status.reports_failed,(unsigned long)status.control_in_completed,
        (unsigned long)status.control_out_completed,(unsigned long)status.error,status.stage,
        gamepad_endpoint_in!=NULL,gamepad_transfer_pending,
        (unsigned long)(gamepad_transfer_pending?gamepad_transfer_id:0),
        (unsigned long long)(gamepad_transfer_pending?now_ms-gamepad_transfer_started:0));
    if(written<0) { *result=(KeyboardLogError){true,errno,"fprintf"};return false; }
    errno=0;
    if(fflush(log_file)==EOF) { *result=(KeyboardLogError){true,errno,"fflush"};return false; }
    return true;
}

static Result failed(const char *stage, Result rc) {
    status.error=rc; snprintf(status.stage,sizeof(status.stage),"%s",stage);
    keyboard_log("ERROR %s: 0x%08lx module=%lu description=%lu",stage,(unsigned long)rc,
        (unsigned long)(rc&0x1ff),(unsigned long)((rc>>9)&0x1fff)); return rc;
}
#define STEP(name, call) do { rc=(call); if (R_FAILED(rc)) { failed(name,rc); goto fail; } } while (0)

/* The gamepad idles at hat-neutral and stick-center, matching PicoSwitch. */
static void gamepad_neutral(SwitchGamepadReport *report) {
    memset(report,0,sizeof(*report));
    report->hat=SWITCH_GAMEPAD_HAT_NONE;
    report->lx=SWITCH_GAMEPAD_STICK_CENTER;
    report->ly=SWITCH_GAMEPAD_STICK_CENTER;
    report->rx=SWITCH_GAMEPAD_STICK_CENTER;
    report->ry=SWITCH_GAMEPAD_STICK_CENTER;
}

static void gamepad_exclusive_interrupt(void) {
    if(gamepad_exclusive!=GAMEPAD_EXCLUSIVE_OFF) {
        gamepad_exclusive=GAMEPAD_EXCLUSIVE_INTERRUPTED;
        gamepad_exclusive_transfer=GAMEPAD_EXCLUSIVE_OFF;
        gamepad_neutral(&gamepad_desired_report);
    }
}

Result keyboard_init(void) {
    if(civ_owned) return KERNELRESULT(InvalidState);
    bool gamepad_descriptor_was_valid=gamepad_descriptor_valid;
    memset(&status,0,sizeof(status)); memset(last_report,0,sizeof(last_report));
    diag_readiness_seen=false;
    diag_submit_logged=diag_completion_logged=diag_failure_logged=false;
    ctrl_in_pending=ctrl_out_pending=led_data_pending=gamepad_data_pending=false;
    ctrl_in_id=ctrl_out_id=0;
    ctrl_in_expected=ctrl_out_expected=0;
    control_intf=ctrl_in_intf=ctrl_out_intf=NULL;
    descriptor_request=descriptor_in_done=descriptor_out_done=false;
    descriptor_intf=NULL;
    gamepad_descriptor_valid=false;
    gamepad_transfer_pending=gamepad_out_pending=false;
    gamepad_error_logged=false;
    usb_configured_ready=false;
    control_read=status_out_after_in=false; configured_at=last_send=0;
    log_milliseconds=log_origin=0; log_origin_set=false;
    status.protocol=1; status.state=UsbState_Detached;
    gamepad_neutral(&gamepad_desired_report);
    gamepad_last_report=gamepad_desired_report;
    errno=0;
    log_file=fopen("sdmc:/switch/C7S-Loader/usb.log","w");
    if(!log_file) { int open_errno=errno;remember_log_error("fopen",open_errno); }
    if(gamepad_descriptor_was_valid)
        keyboard_log("Gamepad descriptor valid=0: keyboard_init reset");
    keyboard_log("C7S-Loader RecurCiv USB | HOS %u.%u.%u | applet=%d",
        HOSVER_MAJOR(hosversionGet()),HOSVER_MINOR(hosversionGet()),HOSVER_MICRO(hosversionGet()),appletGetAppletType());
    Result rc=0;
    if (hosversionBefore(5,0,0)) return failed("Horizon OS 5.0.0 required",MAKERESULT(Module_Libnx,LibnxError_IncompatSysVer));
    STEP("Open usb:ds",usbDsInitialize()); service_open=true;
    STEP("Clear USB descriptors",usbDsClearDeviceData());
    uint16_t langs[]={0x0409}; uint8_t language,manufacturer,product;
    STEP("USB language string",usbDsAddUsbLanguageStringDescriptor(&language,langs,1));
    STEP("USB manufacturer string",usbDsAddUsbStringDescriptor(&manufacturer,"PICO SWITCH PRO."));
    STEP("USB product string",usbDsAddUsbStringDescriptor(&product,"J24 CONTROLLER"));
    /* Experimental PicoSwitch identity; USB layout remains Plume's. */
    struct usb_device_descriptor dev={
        .bLength=18,.bDescriptorType=USB_DT_DEVICE,.bcdUSB=0x0200,.bMaxPacketSize0=64,
        .idVendor=0x0f0d,.idProduct=0x0092,.bcdDevice=0x0100,
        .iManufacturer=manufacturer,.iProduct=product,.iSerialNumber=0,.bNumConfigurations=1
    };
    keyboard_log("EXPERIMENT device VID=%04x PID=%04x bcdDevice=%04x manufacturer=PICO SWITCH PRO. product=J24 CONTROLLER serial=none",
        dev.idVendor,dev.idProduct,dev.bcdDevice);
    STEP("Full Speed descriptor",usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full,&dev));
    STEP("High Speed descriptor",usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High,&dev));
    dev.bcdUSB=0x0300; dev.bMaxPacketSize0=9;
    STEP("Super Speed descriptor",usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super,&dev));
    const uint8_t bos[]={5,15,22,0,2, 7,16,2,2,0,0,0, 10,16,3,0,14,0,1,10,0xff,7};
    STEP("BOS descriptor",usbDsSetBinaryObjectStore(bos,sizeof(bos)));
    /* Gamepad stays interface 0; endpoint numbers follow usb:ds allocation. */
    STEP("Gamepad interface 0",usbDsRegisterInterfaceEx(&gamepad_intf,0));
    /* Plume keyboard keeps its existing report descriptor and logic, now interface 1. */
    STEP("Keyboard interface 1",usbDsRegisterInterfaceEx(&intf,1));

    struct usb_interface_descriptor gamepad_id={
        .bLength=9,.bDescriptorType=USB_DT_INTERFACE,.bInterfaceNumber=0,
        .bNumEndpoints=2,.bInterfaceClass=USB_CLASS_HID,.bInterfaceSubClass=0,.bInterfaceProtocol=0
    };
    struct usb_endpoint_descriptor gamepad_out={
        .bLength=7,.bDescriptorType=USB_DT_ENDPOINT,.bEndpointAddress=0x01,
        .bmAttributes=USB_TRANSFER_TYPE_INTERRUPT,.wMaxPacketSize=64,.bInterval=1
    };
    struct usb_endpoint_descriptor gamepad_in={
        .bLength=7,.bDescriptorType=USB_DT_ENDPOINT,.bEndpointAddress=0x81,
        .bmAttributes=USB_TRANSFER_TYPE_INTERRUPT,.wMaxPacketSize=64,.bInterval=1
    };
    struct usb_interface_descriptor id={
        .bLength=9,.bDescriptorType=USB_DT_INTERFACE,.bInterfaceNumber=1,
        .bNumEndpoints=1,.bInterfaceClass=USB_CLASS_HID,.bInterfaceSubClass=1,.bInterfaceProtocol=1
    };
    struct usb_endpoint_descriptor ep={
        .bLength=7,.bDescriptorType=USB_DT_ENDPOINT,.bEndpointAddress=0x82,
        .bmAttributes=USB_TRANSFER_TYPE_INTERRUPT,.wMaxPacketSize=8,.bInterval=1
    };
    for (unsigned i=0;i<3;i++) {
        UsbDeviceSpeed speed=i==0?UsbDeviceSpeed_Full:i==1?UsbDeviceSpeed_High:UsbDeviceSpeed_Super;
        uint8_t interval=i==0?1:4; /* 1ms at FS and at HS/SS */
        ep.bInterval=gamepad_in.bInterval=gamepad_out.bInterval=interval;
        keyboard_log("EXPERIMENT descriptors speed=%u intf0 HID 03/00/00 report=%lu OUT=%02x IN=%02x packet=%u interval=%u intf1 HID 03/01/01 report=%lu IN=%02x packet=%u interval=%u",
            (unsigned)speed,(unsigned long)sizeof(switch_gamepad_report_descriptor),
            gamepad_out.bEndpointAddress,gamepad_in.bEndpointAddress,gamepad_in.wMaxPacketSize,
            gamepad_in.bInterval,(unsigned long)sizeof(report_descriptor),ep.bEndpointAddress,
            ep.wMaxPacketSize,ep.bInterval);

        STEP("Gamepad interface descriptor",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,&gamepad_id,sizeof(gamepad_id)));
        STEP("Gamepad HID descriptor",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,&gamepad_hid_descriptor,sizeof(gamepad_hid_descriptor)));
        STEP("Gamepad OUT descriptor",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,&gamepad_out,sizeof(gamepad_out)));
        if (speed==UsbDeviceSpeed_Super) {
            const uint8_t companion[]={6,48,0,0,64,0};
            STEP("Gamepad OUT companion",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,companion,sizeof(companion)));
        }
        STEP("Gamepad IN descriptor",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,&gamepad_in,sizeof(gamepad_in)));
        if (speed==UsbDeviceSpeed_Super) {
            const uint8_t companion[]={6,48,0,0,64,0};
            STEP("Gamepad IN companion",usbDsInterface_AppendConfigurationData(gamepad_intf,speed,companion,sizeof(companion)));
        }

        STEP("Keyboard interface descriptor",usbDsInterface_AppendConfigurationData(intf,speed,&id,sizeof(id)));
        STEP("Keyboard HID descriptor",usbDsInterface_AppendConfigurationData(intf,speed,&hid_descriptor,sizeof(hid_descriptor)));
        STEP("Keyboard IN descriptor",usbDsInterface_AppendConfigurationData(intf,speed,&ep,sizeof(ep)));
        if (speed==UsbDeviceSpeed_Super) {
            const uint8_t companion[]={6,48,0,0,8,0};
            STEP("Keyboard IN companion",usbDsInterface_AppendConfigurationData(intf,speed,companion,sizeof(companion)));
        }
    }

    /* usb:ds allocates endpoint numbers as interface_index+1 per direction.
       Register interface 0's endpoints before interface 1 so the allocated
       addresses match the appended descriptors:
       gamepad IN 0x81, gamepad OUT 0x01, keyboard IN 0x82. */
    STEP("Register gamepad IN 81",usbDsInterface_RegisterEndpoint(gamepad_intf,&gamepad_endpoint_in,0x81));
    STEP("Register gamepad OUT 01",usbDsInterface_RegisterEndpoint(gamepad_intf,&gamepad_endpoint_out,0x01));
    STEP("Register keyboard IN 82",usbDsInterface_RegisterEndpoint(intf,&endpoint,0x82));

    STEP("Enable keyboard interface",usbDsInterface_EnableInterface(intf));
    STEP("Enable gamepad interface",usbDsInterface_EnableInterface(gamepad_intf));
    STEP("Enable USB device",usbDsEnable());
    status.initialized=true; snprintf(status.stage,sizeof(status.stage),"USB initialization complete");
    keyboard_log("USB composite enabled | keyboard intf1 EP82 | gamepad intf0 EP81/01");
    return 0;
fail:
    if (service_open) { usbDsExit(); service_open=false; }
    intf=NULL; endpoint=NULL; gamepad_intf=NULL; gamepad_endpoint_in=NULL; gamepad_endpoint_out=NULL;
    control_intf=ctrl_in_intf=ctrl_out_intf=NULL;
    status.initialized=false;
    gamepad_transfer_pending=gamepad_out_pending=false;
    return rc;
}

static bool signalled(Event *event) {
    if (R_FAILED(eventWait(event,0))) return false;
    eventClear(event); return true;
}
static void control_stall(void) {
    UsbDsInterface *owner = ctrl_in_pending ? ctrl_in_intf :
                            ctrl_out_pending ? ctrl_out_intf : control_intf;
    Result rc = owner ? usbDsInterface_StallCtrl(owner) :
                        MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    keyboard_log("Control STALL owner=%s -> %08lx",
        owner==gamepad_intf?"gamepad":owner==intf?"keyboard":"none",
        (unsigned long)rc);
    ctrl_in_pending=ctrl_out_pending=led_data_pending=gamepad_data_pending=false;
    status_out_after_in=false;
    descriptor_request=false;
}
static void queue_status_out(uint64_t now) {
    UsbDsInterface *owner=ctrl_in_intf;
    if(!owner) { keyboard_log("Missing IN transfer owner for status OUT"); return; }
    Result rc=usbDsInterface_CtrlOutPostBufferAsync(owner,ctrl_out,0,&ctrl_out_id);
    if(R_FAILED(rc)) { failed("USB control OUT status",rc);control_stall();return; }
    ctrl_out_intf=owner;ctrl_out_pending=true;ctrl_out_expected=0;control_started=now;
    keyboard_log("CTRL OUT status queued AFTER IN completion urb=%lu bytes=0",(unsigned long)ctrl_out_id);
}
static void reply(const void *data,size_t size,uint16_t requested,uint64_t now) {
    if(!control_intf) { keyboard_log("Missing control interface for response"); return; }
    if(size>requested) size=requested;
    if(size) memcpy(ctrl_in,data,size);
    Result rc;
    /* usb:ds on hardware rejected IN when OUT status had already been queued.
       Submit IN data first; the completion handler advances to OUT status. */
    status_out_after_in=control_read;
    rc=usbDsInterface_CtrlInPostBufferAsync(control_intf,ctrl_in,size,&ctrl_in_id);
    if(R_FAILED(rc)) { failed("USB control IN response",rc); control_stall(); return; }
    ctrl_in_intf=control_intf;ctrl_in_pending=true;ctrl_in_expected=(uint32_t)size;control_started=now;
    keyboard_log("CTRL IN %s queued intf=%s urb=%lu bytes=%lu",control_read?"data":"status",
        control_intf==gamepad_intf?"gamepad":"keyboard",(unsigned long)ctrl_in_id,(unsigned long)size);
}
/* Requests are answered by the interface named in wIndex, not by whichever
   SetupEvent happened to fire first. This also protects the keyboard
   descriptor handshake if usb:ds signals both interface events. */
static UsbDsInterface *setup_target(const struct usb_control_setup *s, UsbDsInterface *source) {
    if((s->bmRequestType&0x1f)!=0x01) return source;
    if(s->wIndex==0) return gamepad_intf;
    if(s->wIndex==1) return intf;
    return NULL;
}
static void handle_setup(UsbDsInterface *source, uint64_t now) {
    struct usb_control_setup s={0};
    Result rc=usbDsInterface_GetSetupPacket(source,&s,sizeof(s));
    if(R_FAILED(rc)) { failed("Read USB setup request",rc); return; }
    if(s.bmRequestType==0 && s.bRequest==0 && s.wValue==0 && s.wIndex==0 && s.wLength==0) return;
    status.setup_count++; status.last_request_type=s.bmRequestType;
    status.last_request=s.bRequest; status.last_value=s.wValue;
    keyboard_log("SETUP type=%02x req=%02x value=%04x index=%04x length=%u source=%s",
        s.bmRequestType,s.bRequest,s.wValue,s.wIndex,s.wLength,
        source==gamepad_intf?"gamepad":"keyboard");
    if(ctrl_out_pending)
        keyboard_log("SETUP supersedes CTRL OUT: expected=%lu owner=%s new_type=%02x new_req=%02x new_index=%04x",
            (unsigned long)ctrl_out_id,ctrl_out_intf==gamepad_intf?"gamepad":ctrl_out_intf==intf?"keyboard":"unknown",
            s.bmRequestType,s.bRequest,s.wIndex);
    ctrl_in_pending=ctrl_out_pending=led_data_pending=gamepad_data_pending=false;
    status_out_after_in=false;
    control_read=(s.bmRequestType&0x80)!=0;
    descriptor_request=false;
    UsbDsInterface *target=setup_target(&s,source);
    control_intf=target?target:source;
    uint8_t type=s.bmRequestType;
    if(type==0x80 && s.bRequest==6 && (s.wValue>>8)==USB_DT_CONFIG) {
        keyboard_log("Ignoring device-level configuration descriptor event; usb:ds handles it");
        return;
    }
    if(!target) { control_stall(); return; }
    if(type==0x81 && s.bRequest==0 && s.wValue==0) {
        const uint16_t flags=0;reply(&flags,2,s.wLength,now); /* GET_STATUS (interface) */
    } else if(type==0x81 && s.bRequest==10 && s.wValue==0) {
        const uint8_t alternate=0;reply(&alternate,1,s.wLength,now); /* GET_INTERFACE */
    } else if(type==0x01 && s.bRequest==11 && s.wValue==0 && s.wLength==0) {
        reply(NULL,0,0,now); /* SET_INTERFACE (only alternate zero) */
    } else if(type==0x81 && s.bRequest==6 && (s.wValue>>8)==USB_DT_REPORT && (s.wValue&255)==0) {
        descriptor_intf=target;
        descriptor_request=true;
        descriptor_in_done=descriptor_out_done=false;
        if(target==gamepad_intf) {
            keyboard_log("Gamepad report descriptor requested; previous valid=%u",gamepad_descriptor_valid);
            reply(switch_gamepad_report_descriptor,sizeof(switch_gamepad_report_descriptor),s.wLength,now);
        } else {
            status.report_requests++;
            status.descriptor_valid=false;
            reply(report_descriptor,sizeof(report_descriptor),s.wLength,now);
        }
    } else if(type==0x81 && s.bRequest==6 && (s.wValue>>8)==USB_DT_HID && (s.wValue&255)==0) {
        if(target==gamepad_intf) reply(&gamepad_hid_descriptor,sizeof(gamepad_hid_descriptor),s.wLength,now);
        else reply(&hid_descriptor,sizeof(hid_descriptor),s.wLength,now);
    } else if(type==0xa1 && s.bRequest==1 && (s.wValue&255)==0) { /* GET_REPORT */
        if((s.wValue>>8)==1) {
            if(target==gamepad_intf) reply(&gamepad_last_report,sizeof(gamepad_last_report),s.wLength,now);
            else reply(last_report,8,s.wLength,now);
        } else if((s.wValue>>8)==2 && target==intf) {
            reply(&status.leds,1,s.wLength,now);
        } else control_stall();
    } else if(type==0xa1 && s.bRequest==2 && (s.wValue&255)==0) { /* GET_IDLE */
        if(target==intf) reply(&status.idle,1,s.wLength,now);
        else { const uint8_t idle=0;reply(&idle,1,s.wLength,now); }
    } else if(type==0xa1 && s.bRequest==3 && target==intf) { /* GET_PROTOCOL (keyboard) */
        reply(&status.protocol,1,s.wLength,now);
    } else if(type==0x21 && s.bRequest==10 && s.wLength==0 && (s.wValue&255)==0) { /* SET_IDLE */
        if(target==intf) status.idle=s.wValue>>8;
        reply(NULL,0,0,now);
    } else if(type==0x21 && s.bRequest==11 && target==intf && s.wLength==0 && s.wValue<=1) { /* SET_PROTOCOL */
        status.protocol=(uint8_t)s.wValue; reply(NULL,0,0,now);
    } else if(type==0x21 && s.bRequest==9 && s.wValue==0x0200 && target==intf && s.wLength==1) {
        memset(ctrl_out,0,sizeof(ctrl_out));
        rc=usbDsInterface_CtrlOutPostBufferAsync(control_intf,ctrl_out,1,&ctrl_out_id);
        if(R_FAILED(rc)) { failed("Receive keyboard LEDs",rc); control_stall(); }
        else {
            ctrl_out_intf=control_intf;ctrl_out_pending=led_data_pending=true;ctrl_out_expected=1;control_started=now;
            keyboard_log("CTRL OUT LED data queued urb=%lu bytes=1",(unsigned long)ctrl_out_id);
        }
    } else if(type==0x21 && s.bRequest==9 && s.wValue==0x0200 && target==gamepad_intf && s.wLength==8) {
        memset(ctrl_out,0,sizeof(ctrl_out));
        rc=usbDsInterface_CtrlOutPostBufferAsync(control_intf,ctrl_out,8,&ctrl_out_id);
        if(R_FAILED(rc)) { failed("Receive gamepad output report",rc); control_stall(); }
        else {
            ctrl_out_intf=control_intf;ctrl_out_pending=gamepad_data_pending=true;ctrl_out_expected=8;control_started=now;
            gamepad_ctrl_generation=gamepad_owner_generation;
            keyboard_log("CTRL OUT gamepad data queued urb=%lu bytes=8",(unsigned long)ctrl_out_id);
        }
    } else control_stall();
}

static void keyboard_cancel_internal(void) {
    if(endpoint && status.transfer_pending) usbDsEndpoint_Cancel(endpoint);
    status.transfer_pending=false;
    /* The buffer must not be reused until Cancel has returned. */
    if(endpoint) {
        UsbDsReportData reports;
        eventClear(&endpoint->CompletionEvent);
        usbDsEndpoint_GetReportData(endpoint,&reports);
    }
}
void keyboard_cancel_transfer(void) {
    if(!civ_owned) keyboard_cancel_internal();
}

static void gamepad_cancel_transfers(void) {
    if(gamepad_endpoint_in && gamepad_transfer_pending) usbDsEndpoint_Cancel(gamepad_endpoint_in);
    gamepad_transfer_pending=false;
    if(gamepad_endpoint_in) {
        /* Drop the stale completion so it cannot retire a later transfer. */
        UsbDsReportData reports;
        eventClear(&gamepad_endpoint_in->CompletionEvent);
        usbDsEndpoint_GetReportData(gamepad_endpoint_in,&reports);
    }
    if(gamepad_endpoint_out && gamepad_out_pending) usbDsEndpoint_Cancel(gamepad_endpoint_out);
    gamepad_out_pending=false;
    if(gamepad_endpoint_out) {
        UsbDsReportData reports;
        eventClear(&gamepad_endpoint_out->CompletionEvent);
        usbDsEndpoint_GetReportData(gamepad_endpoint_out,&reports);
    }
}

void keyboard_poll(uint64_t now) {
    if(!status.initialized) return;
    if(!log_origin_set) { log_origin=now;log_origin_set=true; }
    log_milliseconds=now-log_origin;
    UsbState state;
    Result rc=usbDsGetState(&state);
    if(R_FAILED(rc)) { failed("Read USB state",rc); status.ready=false;usb_configured_ready=false;civ_interrupted();return; }
    if(state!=status.state) {
        keyboard_log("USB state %u -> %u",status.state,state);
        if(status.state==UsbState_Configured)
            keyboard_log("Leaving Configured: configured_at=%llu elapsed=%llu ms",
                (unsigned long long)configured_at,
                (unsigned long long)(now-configured_at));
        if(status.state==UsbState_Configured && state==UsbState_Default) status.bus_resets++;
        status.state=state;
        if(state==UsbState_Configured) {
            configured_at=now;
            diag_submit_logged=diag_completion_logged=diag_failure_logged=false;
        }
        if(state!=UsbState_Configured) {
            if(civ_owned) { civ_interrupted();usb_civ_abort(civ_transfer.ticket); }
            else keyboard_cancel_internal();
            if(gamepad_transfer_pending)
                keyboard_log("EP81 cancel: USB state transition to %u urb=%lu",
                    state,(unsigned long)gamepad_transfer_id);
            if(!civ_owned) gamepad_cancel_transfers();
            gamepad_exclusive_interrupt();
            ctrl_in_pending=ctrl_out_pending=led_data_pending=gamepad_data_pending=false;
            status_out_after_in=false;
            descriptor_request=descriptor_in_done=descriptor_out_done=false;
            if(state!=UsbState_Suspended) {
                status.descriptor_valid=false;
                if(gamepad_descriptor_valid)
                    keyboard_log("Gamepad descriptor valid=0: USB state transition to %u",state);
                gamepad_descriptor_valid=false;
                gamepad_neutral(&gamepad_desired_report);
                gamepad_last_report=gamepad_desired_report;
            }
            if(state!=UsbState_Suspended) { status.leds=0;status.leds_known=false;status.protocol=1;status.idle=0; }
        }
    }
    /* Retire old completions before reading a new SETUP packet. A new SETUP
       legally supersedes an earlier control request. Never match it to an old URB. */
    if(ctrl_in_intf && signalled(&ctrl_in_intf->CtrlInCompletionEvent)) {
        UsbDsReportData reports;
        rc=usbDsInterface_GetCtrlInReportData(ctrl_in_intf,&reports);
        if(R_SUCCEEDED(rc)) {
            bool found=false;
            for(uint32_t i=0;i<reports.report_count && i<8;i++) {
                const UsbDsReportEntry *r=&reports.report[i];
                keyboard_log("CTRL IN done urb=%lu requested=%lu actual=%lu status=%lu",
                    (unsigned long)r->id,(unsigned long)r->requestedSize,(unsigned long)r->transferredSize,(unsigned long)r->urb_status);
                if(ctrl_in_pending && r->id==ctrl_in_id) found=true;
            }
            if(found) {
                uint32_t actual=0;rc=usbDsParseReportData(&reports,ctrl_in_id,NULL,&actual);
                ctrl_in_pending=false;
                if(R_SUCCEEDED(rc) && actual==ctrl_in_expected) {
                    status.control_in_completed++;
                    if(descriptor_request) descriptor_in_done=true;
                    if(status_out_after_in) {
                        status_out_after_in=false;
                        queue_status_out(now);
                    }
                } else { failed("USB control IN completion",R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_BadUsbCommsWrite));control_stall(); }
            }
        } else failed("Read USB control IN result",rc);
    }
    if(ctrl_out_intf && signalled(&ctrl_out_intf->CtrlOutCompletionEvent)) {
        UsbDsReportData reports; uint32_t transferred=0;
        rc=usbDsInterface_GetCtrlOutReportData(ctrl_out_intf,&reports);
        if(R_SUCCEEDED(rc)) {
            bool found=false;
            for(uint32_t i=0;i<reports.report_count && i<8;i++) {
                const UsbDsReportEntry *r=&reports.report[i];
                keyboard_log("CTRL OUT done urb=%lu expected=%lu pending=%u owner=%s match=%u requested=%lu actual=%lu status=%lu",
                    (unsigned long)r->id,(unsigned long)ctrl_out_id,ctrl_out_pending,
                    ctrl_out_intf==gamepad_intf?"gamepad":ctrl_out_intf==intf?"keyboard":"unknown",
                    ctrl_out_pending && r->id==ctrl_out_id,(unsigned long)r->requestedSize,
                    (unsigned long)r->transferredSize,(unsigned long)r->urb_status);
                if(ctrl_out_pending && r->id==ctrl_out_id) found=true;
            }
            if(found) {
                rc=usbDsParseReportData(&reports,ctrl_out_id,NULL,&transferred);
                ctrl_out_pending=false;
                if(R_SUCCEEDED(rc) && transferred==ctrl_out_expected) {
                    status.control_out_completed++;
                    if(led_data_pending) {
                        led_data_pending=false;armDCacheFlush(ctrl_out,sizeof(ctrl_out));status.leds=ctrl_out[0];status.leds_known=true;
                        keyboard_log("LED report=%02x",status.leds);control_intf=ctrl_out_intf;reply(NULL,0,0,now);
                    } else if(gamepad_data_pending) {
                        gamepad_data_pending=false;armDCacheFlush(ctrl_out,sizeof(ctrl_out));
                        if(ctrl_out_expected>=sizeof(SwitchGamepadReport) && !civ_owned && gamepad_exclusive==GAMEPAD_EXCLUSIVE_OFF && gamepad_ctrl_generation==gamepad_owner_generation) {
                            memcpy(&gamepad_desired_report,ctrl_out,sizeof(SwitchGamepadReport));
                            gamepad_last_report=gamepad_desired_report;
                            keyboard_log("Gamepad output report echoed: buttons=%04x hat=%02x",
                                gamepad_desired_report.buttons,gamepad_desired_report.hat);
                        }
                        control_intf=ctrl_out_intf;reply(NULL,0,0,now);
                    } else if(descriptor_request) descriptor_out_done=true;
                } else { failed("USB control OUT completion",R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_BadUsbCommsWrite));control_stall(); }
            }
        } else failed("Read USB control OUT result",rc);
    }
    if(descriptor_request && descriptor_in_done && descriptor_out_done) {
        if(descriptor_intf==gamepad_intf) {
            if(!gamepad_descriptor_valid) { gamepad_descriptor_valid=true;keyboard_log("Gamepad descriptor valid=1: matched successful descriptor IN and OUT status"); }
        } else if(descriptor_intf==intf) {
            if(!status.descriptor_valid) { status.descriptor_valid=true;keyboard_log("Keyboard HID report descriptor acknowledged"); }
        }
        descriptor_request=false;
    }
    /* usb:ds may signal both interface SetupEvents for one request. Consume
       both events, but handle the SETUP once; setup_target() routes by wIndex
       so the reply goes to the interface named in the request. */
    bool gamepad_setup=signalled(&gamepad_intf->SetupEvent);
    bool keyboard_setup=signalled(&intf->SetupEvent);
    if(gamepad_setup || keyboard_setup)
        handle_setup(gamepad_setup?gamepad_intf:intf,now);
    if((ctrl_in_pending || ctrl_out_pending) && now-control_started>1500) {
        keyboard_log("Control transfer timeout IN=%u OUT=%u",ctrl_in_pending,ctrl_out_pending); control_stall();
    }
    usb_configured_ready=state==UsbState_Configured && now-configured_at>=300;
    bool gp_ready=gamepad_ready();
    if(!diag_readiness_seen ||
       diag_configured_ready!=usb_configured_ready ||
       diag_keyboard_desc!=status.descriptor_valid ||
       diag_gamepad_desc!=gamepad_descriptor_valid ||
       diag_gamepad_ready!=gp_ready) {
        keyboard_log("Readiness state=%u configured_ready=%u keyboard_desc=%u gamepad_desc=%u gamepad_ready=%u",
            state,usb_configured_ready,status.descriptor_valid,
            gamepad_descriptor_valid,gp_ready);
        if(state==UsbState_Configured)
            keyboard_log("Configured timing: configured_at=%llu elapsed=%llu ms",
                (unsigned long long)configured_at,
                (unsigned long long)(now-configured_at));
        if(!diag_readiness_seen || diag_gamepad_ready!=gp_ready) {
            diag_submit_logged=false;
            diag_completion_logged=false;
            diag_failure_logged=false;
        }
        diag_readiness_seen=true;
        diag_configured_ready=usb_configured_ready;
        diag_keyboard_desc=status.descriptor_valid;
        diag_gamepad_desc=gamepad_descriptor_valid;
        diag_gamepad_ready=gp_ready;
    }
    bool ready=usb_configured_ready && status.descriptor_valid;
    if(ready!=status.ready) keyboard_log("HID ready=%u descriptor_valid=%u resets=%lu",ready,status.descriptor_valid,(unsigned long)status.bus_resets);
    status.ready=ready;
    if(civ_owned) {
        civ_poll_input(endpoint,transfer_id,&status.transfer_pending,transfer_started,now,1);
        return;
    }
    if(signalled(&endpoint->CompletionEvent)) {
        UsbDsReportData reports; uint32_t transferred=0;
        rc=usbDsEndpoint_GetReportData(endpoint,&reports);
        if(R_SUCCEEDED(rc) && status.transfer_pending) rc=usbDsParseReportData(&reports,transfer_id,NULL,&transferred);
        if(status.transfer_pending) {
            status.transfer_pending=false;
            if(R_FAILED(rc) || transferred!=8) {
                status.reports_failed++; failed("Keyboard report completion",R_FAILED(rc)?rc:MAKERESULT(Module_Libnx,LibnxError_BadUsbCommsWrite));
            } else { status.reports_completed++; memcpy(last_report,transfer_buffer,8); }
        }
    }
    if(status.transfer_pending && now-transfer_started>2500) {
        keyboard_cancel_transfer(); status.reports_failed++;
        failed("Keyboard report timed out",KERNELRESULT(TimedOut));
    }
}

static Result keyboard_send_internal(uint8_t modifier,uint8_t key,uint64_t now) {
    if(!status.ready || status.transfer_pending) return KERNELRESULT(InvalidState);
    memset(transfer_buffer,0,8); transfer_buffer[0]=modifier; transfer_buffer[2]=key;
    Result rc=usbDsEndpoint_PostBufferAsync(endpoint,transfer_buffer,8,&transfer_id);
    if(R_FAILED(rc)) { status.reports_failed++; return failed("Send keyboard report",rc); }
    transfer_started=last_send=now; status.transfer_pending=true; return 0;
}
Result keyboard_send(uint8_t modifier,uint8_t key,uint64_t now) {
    if(civ_owned) return KERNELRESULT(InvalidState);
    return keyboard_send_internal(modifier,key,now);
}
bool keyboard_idle_due(uint64_t now) {
    return !civ_owned && status.ready && !status.transfer_pending && status.idle && now-last_send>=(uint64_t)status.idle*4;
}
const KeyboardStatus *keyboard_status(void) { return &status; }
void keyboard_exit(void) {
    if(civ_owned) return; /* Forced teardown must use usb_civ_shutdown. */
    gamepad_exclusive_interrupt();
    keyboard_log("EXIT completed=%lu failed=%lu ctrl_in=%lu ctrl_out=%lu resets=%lu",
        (unsigned long)status.reports_completed,(unsigned long)status.reports_failed,
        (unsigned long)status.control_in_completed,(unsigned long)status.control_out_completed,(unsigned long)status.bus_resets);
    if(service_open) {
        keyboard_cancel_transfer();
        if(gamepad_transfer_pending)
            keyboard_log("EP81 cancel: keyboard_exit urb=%lu",(unsigned long)gamepad_transfer_id);
        gamepad_cancel_transfers();
        usbDsExit();
    }
    service_open=false; intf=NULL; endpoint=NULL; gamepad_intf=NULL; gamepad_endpoint_in=NULL; gamepad_endpoint_out=NULL;
    control_intf=ctrl_in_intf=ctrl_out_intf=NULL;
    status.initialized=false;
    ctrl_in_pending=ctrl_out_pending=led_data_pending=gamepad_data_pending=false;
    status_out_after_in=false;
    if(log_file) fclose(log_file);
    log_file=NULL;
}

/* Gamepad interface: PicoSwitch descriptor, continuous neutral/current stream. */
Result gamepad_init(void) {
    if(civ_owned) return KERNELRESULT(InvalidState);
    if (!status.initialized || !gamepad_endpoint_in)
        return KERNELRESULT(InvalidState);
    gamepad_neutral(&gamepad_desired_report);
    gamepad_last_report=gamepad_desired_report;
    gamepad_transfer_pending=false;
    gamepad_out_pending=false;
    gamepad_error_logged=false;
    gamepad_exclusive_interrupt();
    gamepad_exclusive_transfer=GAMEPAD_EXCLUSIVE_OFF;
    gamepad_owner_generation++;
    keyboard_log("Gamepad neutral stream prepared");
    return 0;
}

const GamepadStatus *gamepad_status(void) {
    static GamepadStatus snapshot;
    snapshot.configured = usb_configured_ready;
    snapshot.descriptor_valid = gamepad_descriptor_valid;
    snapshot.endpoint_available = gamepad_endpoint_in != NULL;
    snapshot.transfer_pending = gamepad_transfer_pending;
    return &snapshot;
}
bool gamepad_ready(void) {
    return usb_configured_ready && gamepad_endpoint_in != NULL;
}

void gamepad_set_state(const SwitchGamepadReport *report) {
    if(!report || civ_owned || gamepad_exclusive!=GAMEPAD_EXCLUSIVE_OFF) return;
    if(memcmp(&gamepad_desired_report,report,sizeof(*report))==0) return;
    gamepad_desired_report=*report;
    keyboard_log("Gamepad state buttons=%04x hat=%02x sticks=%02x/%02x/%02x/%02x",
        gamepad_desired_report.buttons,gamepad_desired_report.hat,
        gamepad_desired_report.lx,gamepad_desired_report.ly,
        gamepad_desired_report.rx,gamepad_desired_report.ry);
}

GamepadExclusiveState gamepad_exclusive_status(void) { return gamepad_exclusive; }

bool gamepad_exclusive_begin(void) {
    if(civ_owned || gamepad_exclusive!=GAMEPAD_EXCLUSIVE_OFF || !gamepad_ready()) return false;
    gamepad_exclusive=GAMEPAD_EXCLUSIVE_SYNC;
    gamepad_owner_generation++;
    gamepad_exclusive_transfer=GAMEPAD_EXCLUSIVE_OFF;
    gamepad_neutral(&gamepad_desired_report);
    return true;
}

bool gamepad_exclusive_submit(const SwitchGamepadReport *report) {
    if(civ_owned || !report || gamepad_exclusive!=GAMEPAD_EXCLUSIVE_READY || !gamepad_ready()) return false;
    gamepad_desired_report=*report;
    gamepad_exclusive=GAMEPAD_EXCLUSIVE_PENDING;
    return true;
}

bool gamepad_exclusive_stop(void) {
    if(civ_owned || gamepad_exclusive==GAMEPAD_EXCLUSIVE_OFF || !gamepad_ready()) return false;
    gamepad_neutral(&gamepad_desired_report);
    gamepad_exclusive=GAMEPAD_EXCLUSIVE_STOPPING;
    return true;
}

void gamepad_poll(uint64_t now_ms) {
    if (!gamepad_endpoint_in) return;

    if(civ_owned) civ_poll_input(gamepad_endpoint_in,gamepad_transfer_id,&gamepad_transfer_pending,
                                 gamepad_transfer_started,now_ms,2);
    if (!civ_owned && gamepad_transfer_pending && signalled(&gamepad_endpoint_in->CompletionEvent)) {
        UsbDsReportData reports;
        uint32_t transferred = 0;
        Result rc = usbDsEndpoint_GetReportData(gamepad_endpoint_in, &reports);
        bool found = false;
        if (R_SUCCEEDED(rc)) {
            for (uint32_t i = 0; i < reports.report_count && i < 8; i++) {
                if (reports.report[i].id == gamepad_transfer_id)
                    found = true;
            }
        }
        if (found) {
            if (!diag_completion_logged) {
                for (uint32_t i=0; i<reports.report_count && i<8; i++) {
                    if (reports.report[i].id==gamepad_transfer_id) {
                        keyboard_log("EP81 completion urb=%lu requested=%lu actual=%lu status=%lu",
                            (unsigned long)reports.report[i].id,
                            (unsigned long)reports.report[i].requestedSize,
                            (unsigned long)reports.report[i].transferredSize,
                            (unsigned long)reports.report[i].urb_status);
                        break;
                    }
                }
                diag_completion_logged=true;
            }
            rc = usbDsParseReportData(&reports, gamepad_transfer_id, NULL, &transferred);
            gamepad_transfer_pending = false;
            if (R_FAILED(rc) || transferred != sizeof(SwitchGamepadReport)) {
                gamepad_exclusive_interrupt();
                if (!gamepad_error_logged) {
                    keyboard_log("Gamepad report failed: rc=%08lx bytes=%lu", (unsigned long)rc, (unsigned long)transferred);
                    gamepad_error_logged = true;
                }
            } else {
                gamepad_error_logged = false;
                if(gamepad_exclusive_transfer==gamepad_exclusive) {
                    if(gamepad_exclusive==GAMEPAD_EXCLUSIVE_SYNC || gamepad_exclusive==GAMEPAD_EXCLUSIVE_PENDING)
                        gamepad_exclusive=GAMEPAD_EXCLUSIVE_READY;
                    else if(gamepad_exclusive==GAMEPAD_EXCLUSIVE_STOPPING) {
                        gamepad_exclusive=GAMEPAD_EXCLUSIVE_OFF;
                        gamepad_owner_generation++;
                    }
                }
            }
            gamepad_exclusive_transfer=GAMEPAD_EXCLUSIVE_OFF;
        } else if (R_SUCCEEDED(rc)) {
            /* A late report from a cancelled URB. Keep the current URB pending;
               retiring it here would post a replacement onto the same DMA buffer
               while the current transfer is still in flight. */
            keyboard_log("EP81 report read succeeded but no matching URB: pending=%lu",
                (unsigned long)gamepad_transfer_id);
        } else {
            keyboard_log("EP81 pending cleared: report read failed urb=%lu rc=%08lx",
                (unsigned long)gamepad_transfer_id,(unsigned long)rc);
            gamepad_transfer_pending = false;
            gamepad_exclusive_interrupt();
            if (!gamepad_error_logged) {
                keyboard_log("Gamepad report read failed: rc=%08lx", (unsigned long)rc);
                gamepad_error_logged = true;
            }
        }
    }
    if (!civ_owned && gamepad_transfer_pending && now_ms - gamepad_transfer_started > 2500) {
        keyboard_log("EP81 timeout: urb=%lu pending_age=%llu ms; cancelling",
            (unsigned long)gamepad_transfer_id,
            (unsigned long long)(now_ms-gamepad_transfer_started));
        usbDsEndpoint_Cancel(gamepad_endpoint_in);
        gamepad_exclusive_interrupt();
        gamepad_transfer_pending = false;
        diag_submit_logged=false;
        diag_completion_logged=false;
        /* Match gamepad_cancel_transfers(): clear and drain cancellation data
           before the buffer can be reused by the next URB. */
        UsbDsReportData reports;
        eventClear(&gamepad_endpoint_in->CompletionEvent);
        usbDsEndpoint_GetReportData(gamepad_endpoint_in, &reports);
        keyboard_log("Gamepad report timed out and drained");
    }

    /* Interrupt OUT 0x01 is kept armed; an output report becomes the next
       input report, matching PicoSwitch's tud_hid_set_report_cb echo. */
    if (gamepad_endpoint_out && gamepad_out_pending && signalled(&gamepad_endpoint_out->CompletionEvent)) {
        UsbDsReportData reports;
        uint32_t transferred = 0;
        Result rc = usbDsEndpoint_GetReportData(gamepad_endpoint_out, &reports);
        bool found = false;
        if (R_SUCCEEDED(rc)) {
            for (uint32_t i = 0; i < reports.report_count && i < 8; i++) {
                if (reports.report[i].id == gamepad_out_id)
                    found = true;
            }
            if (found) rc = usbDsParseReportData(&reports, gamepad_out_id, NULL, &transferred);
            else rc = MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsWrite);
        }
        gamepad_out_pending = false;
        if (R_SUCCEEDED(rc) && transferred >= sizeof(SwitchGamepadReport) && !civ_owned && gamepad_exclusive==GAMEPAD_EXCLUSIVE_OFF && gamepad_out_generation==gamepad_owner_generation) {
            armDCacheFlush(gamepad_out_buffer, sizeof(gamepad_out_buffer));
            memcpy(&gamepad_desired_report, gamepad_out_buffer, sizeof(SwitchGamepadReport));
            gamepad_last_report = gamepad_desired_report;
            keyboard_log("Gamepad interrupt OUT echoed: buttons=%04x hat=%02x",
                gamepad_desired_report.buttons, gamepad_desired_report.hat);
        } else if(R_FAILED(rc) || transferred < sizeof(SwitchGamepadReport)) {
            keyboard_log("Gamepad interrupt OUT failed: rc=%08lx bytes=%lu", (unsigned long)rc, (unsigned long)transferred);
        }
    }
    if (gamepad_endpoint_out && !gamepad_out_pending && gamepad_ready()) {
        Result rc = usbDsEndpoint_PostBufferAsync(gamepad_endpoint_out, gamepad_out_buffer,
                                                  64, &gamepad_out_id);
        if (R_FAILED(rc)) {
            if (!gamepad_error_logged) { failed("Arm gamepad OUT endpoint", rc); gamepad_error_logged = true; }
        } else {
            gamepad_out_pending = true;
            gamepad_out_generation=gamepad_owner_generation;
        }
    }

    /* Continuous stream: re-post the current state whenever the endpoint is
       idle and the device is configured. This matches PicoSwitch's loop. */
    if (!civ_owned && !gamepad_transfer_pending && gamepad_ready()) {
        if (!diag_submit_logged)
            keyboard_log("EP81 submission attempt: bytes=%lu",
                (unsigned long)sizeof(gamepad_desired_report));
        memcpy(gamepad_transfer_buffer, &gamepad_desired_report, sizeof(gamepad_desired_report));
        Result rc = usbDsEndpoint_PostBufferAsync(gamepad_endpoint_in, gamepad_transfer_buffer,
                                                  sizeof(gamepad_desired_report), &gamepad_transfer_id);
        if (!diag_submit_logged) {
            if (R_SUCCEEDED(rc))
                keyboard_log("EP81 submission succeeded: rc=%08lx urb=%lu pending=1",
                    (unsigned long)rc,(unsigned long)gamepad_transfer_id);
            else
                keyboard_log("EP81 submission failed: rc=%08lx pending=0",
                    (unsigned long)rc);
            diag_submit_logged=true;
            diag_failure_logged=R_FAILED(rc);
            diag_failure_rc=rc;
        } else if (R_FAILED(rc) &&
                   (!diag_failure_logged || diag_failure_rc!=rc)) {
            keyboard_log("EP81 submission failed: rc=%08lx pending=0",
                (unsigned long)rc);
            diag_failure_logged=true;
            diag_failure_rc=rc;
        } else if (R_SUCCEEDED(rc) && diag_failure_logged) {
            keyboard_log("EP81 submission recovered: rc=%08lx urb=%lu pending=1",
                (unsigned long)rc,(unsigned long)gamepad_transfer_id);
            diag_failure_logged=false;
        }
        if (R_FAILED(rc)) {
            gamepad_exclusive_interrupt();
            if (!gamepad_error_logged) { failed("Send gamepad report", rc); gamepad_error_logged = true; }
        } else {
            gamepad_last_report = gamepad_desired_report;
            gamepad_transfer_started = now_ms;
            gamepad_transfer_pending = true;
            gamepad_exclusive_transfer=gamepad_exclusive;
            gamepad_error_logged = false;
        }
    }
}

Result gamepad_send(const SwitchGamepadReport *report, uint64_t now_ms) {
    if (civ_owned || !report || !gamepad_ready() || gamepad_transfer_pending || gamepad_exclusive!=GAMEPAD_EXCLUSIVE_OFF)
        return KERNELRESULT(InvalidState);
    gamepad_desired_report = *report;
    memset(gamepad_transfer_buffer, 0, sizeof(gamepad_transfer_buffer));
    memcpy(gamepad_transfer_buffer, report, sizeof(*report));
    Result rc = usbDsEndpoint_PostBufferAsync(
        gamepad_endpoint_in, gamepad_transfer_buffer, sizeof(*report), &gamepad_transfer_id);
    if (R_FAILED(rc)) return failed("Send gamepad report", rc);
    gamepad_last_report = *report;
    gamepad_transfer_started = now_ms;
    gamepad_transfer_pending = true;
    gamepad_error_logged = false;
    return 0;
}

void gamepad_exit(void) {
    if(civ_owned) return;
    gamepad_exclusive_interrupt();
    if(gamepad_transfer_pending)
        keyboard_log("EP81 cancel: gamepad_exit urb=%lu",(unsigned long)gamepad_transfer_id);
    gamepad_cancel_transfers();
}

bool usb_civ_owned(void) { return civ_owned; }
bool usb_civ_ready(void) { return status.initialized && status.ready && gamepad_ready(); }
bool usb_civ_idle(void) {
    return civ_owned && !status.transfer_pending && !gamepad_transfer_pending && !civ_transfer.fence_blocked;
}
UsbCivTransfer usb_civ_transfer(void) { return civ_transfer; }

/* A failed Cancel/drain never licenses DMA-buffer reuse. A later real matching
   completion, or an explicit cleanup retry, can establish the fence instead. */
static bool civ_fence(UsbDsEndpoint *ep,bool *pending,unsigned bit) {
    if(!*pending) return true;
    if(!ep || R_FAILED(usbDsEndpoint_Cancel(ep))) return false;
    UsbDsReportData reports;
    if(R_FAILED(usbDsEndpoint_GetReportData(ep,&reports))) return false;
    eventClear(&ep->CompletionEvent);*pending=false;civ_unknown_submission&=~bit;return true;
}
void usb_civ_abort(uint64_t ticket) {
    if(!civ_owned || ticket!=civ_transfer.ticket) return;
    civ_aborted=true;
    bool k=civ_fence(endpoint,&status.transfer_pending,1);
    bool g=civ_fence(gamepad_endpoint_in,&gamepad_transfer_pending,2);
    civ_transfer.fence_blocked=!k || !g;
}
bool usb_civ_acquire(void) {
    if(civ_owned || !usb_civ_ready() || status.transfer_pending || gamepad_exclusive!=GAMEPAD_EXCLUSIVE_OFF) return false;
    civ_owned=true;gamepad_owner_generation++;
    civ_transfer=(UsbCivTransfer){0};civ_unknown_submission=0;
    usb_civ_abort(0); /* Fence the ordinary continuous controller stream first. */
    return true; /* Lease accepted even if fencing needs explicit recovery. */
}
static void civ_poll_input(UsbDsEndpoint *ep,uint32_t id,bool *pending,
                           uint64_t started,uint64_t now,unsigned bit) {
    if(!ep || !*pending) return;
    if(civ_unknown_submission&bit) return; /* Only Cancel/drain can fence it. */
    if(signalled(&ep->CompletionEvent)) {
        UsbDsReportData reports;uint32_t actual=0;bool found=false;
        Result rc=usbDsEndpoint_GetReportData(ep,&reports);
        if(R_SUCCEEDED(rc)) {
            for(uint32_t i=0;i<reports.report_count && i<8;i++) if(reports.report[i].id==id) found=true;
            if(found) {
                rc=usbDsParseReportData(&reports,id,NULL,&actual);*pending=false;
                if(R_SUCCEEDED(rc) && actual==8) {
                    if(!civ_aborted) civ_transfer.completed|=bit;
                    if(bit==1) { status.reports_completed++;memcpy(last_report,transfer_buffer,8); }
                } else {
                    if(civ_transfer.result==USB_CIV_OK) civ_transfer.result=USB_CIV_FAILED;
                    if(bit==1) status.reports_failed++;
                }
            } /* Stale URB: do not retire/reuse the current buffer. */
        } else if(civ_transfer.result==USB_CIV_OK) civ_transfer.result=USB_CIV_FAILED;
    }
    if(*pending && now>=started && now-started>2500 && civ_transfer.result==USB_CIV_OK)
        civ_transfer.result=USB_CIV_FAILED;
    if(!status.transfer_pending && !gamepad_transfer_pending) civ_transfer.fence_blocked=false;
}
bool usb_civ_submit(uint64_t ticket,uint8_t modifier,uint8_t key,
                    const SwitchGamepadReport *report,uint64_t now) {
    if(!ticket || !report || !usb_civ_ready() || !usb_civ_idle()) return false;
    civ_transfer=(UsbCivTransfer){ticket,0,USB_CIV_OK,false};civ_aborted=false;
    /* Both reports copied to separate, existing aligned DMA buffers. A partial
       submission is ambiguous and MUST be fenced, never replayed. */
    if(R_FAILED(keyboard_send_internal(modifier,key,now))) {
        /* Even an ambiguous failed post must be cancelled before buffer reuse. */
        status.transfer_pending=true;transfer_started=now;civ_unknown_submission|=1;
        civ_transfer.result=USB_CIV_FAILED;return false;
    }
    gamepad_desired_report=*report;
    memcpy(gamepad_transfer_buffer,report,sizeof(*report));
    gamepad_transfer_pending=true;gamepad_transfer_started=now;
    if(R_FAILED(usbDsEndpoint_PostBufferAsync(gamepad_endpoint_in,gamepad_transfer_buffer,8,&gamepad_transfer_id))) {
        civ_unknown_submission|=2;
        civ_transfer.result=USB_CIV_FAILED;return false;
    }
    gamepad_last_report=*report;
    return true;
}
bool usb_civ_release(void) {
    SwitchGamepadReport neutral;gamepad_neutral(&neutral);
    const uint8_t released[8]={0};
    if(!usb_civ_idle() || !usb_civ_ready() || civ_aborted || civ_transfer.result!=USB_CIV_OK ||
       civ_transfer.completed!=3 || memcmp(last_report,released,8) ||
       memcmp(&gamepad_last_report,&neutral,sizeof(neutral))) return false;
    civ_owned=false;gamepad_owner_generation++;return true;
}
void usb_civ_shutdown(void) {
    if(!civ_owned) return;
    keyboard_log("CIV forced shutdown: neutral delivery unresolved; closing usb:ds");
    usb_civ_abort(civ_transfer.ticket);
    /* Service close is the final lifetime fence even if endpoint Cancel failed. */
    if(service_open) usbDsExit();
    service_open=false;status.transfer_pending=gamepad_transfer_pending=gamepad_out_pending=false;
    civ_unknown_submission=0;
    status.ready=false;usb_configured_ready=false;
    civ_owned=false;keyboard_exit();
}
