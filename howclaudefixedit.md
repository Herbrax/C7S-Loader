# How the Switch 2 USB problem was fixed

A record of why C7S-Loader worked on the original Switch but registered nothing on
the Switch 2, how [RecurCIV](https://github.com/OR3OTHUG/RecurCIV) pointed to the
real cause, and exactly what changed in [source/usb_composite.c](source/usb_composite.c).

## The symptom

The loader presents itself over USB as a **composite HID device**: interface 0 is a
HORI-style gamepad, interface 1 is a boot keyboard. A payload run types text on the
keyboard and presses the controller **A** button to navigate.

On the original Switch this worked. On the Switch 2, plugged into the same app,
nothing registered — no keystrokes, no button presses — even though the web UI
showed the device as healthy.

## What the live diagnostics actually said

Pulling `/api/debug` off the running console was important because it ruled out the
obvious guesses:

- `state:5` (`UsbState_Configured`), `error:0`, `bus_resets:0` — the Switch 2 *did*
  enumerate and configure the device.
- After triggering the keyboard test: `kbd_sent:22`, `reports_failed:0` — libnx
  accepted every report and the host polled and drained the interrupt-IN endpoint.
- `leds:1` — the Switch 2 even sent a keyboard LED `SET_REPORT`.

So the bytes were leaving the console and the host was talking to the device. The
failure was not "device not seen" or "transfers failing."

## Where we were mistaken

Several plausible theories were wrong, and it's worth writing them down so they
don't get re-litigated:

1. **"Switch 2 controller authentication is rejecting the HORI identity."**
   Tempting, because the Switch 2 did tighten controller recognition. But the device
   enumerated and configured fine, and the gamepad report descriptor and report
   struct were already **byte-for-byte identical** to RecurCIV's working PicoSwitch
   ones. The identity was never the problem.

2. **"You can't be a licensed controller *and* a keyboard on one USB gadget on
   Switch 2 — the controller driver claims the whole device and starves the
   keyboard."** This was the main wrong turn. It fit the evidence at the time
   (keyboard alone worked, gamepad alone worked, composite didn't) and led to the
   conclusion that the mixed macro was simply impossible on Switch 2. It is not:
   RecurCIV ships the *same* HORI identity with the *same* composite
   gamepad+keyboard layout and both halves work.

3. **"Drop the gamepad and remap A to keyboard Enter/Space."** A reasonable fallback
   *if* theory 2 had been true. It was a dead end anyway — the target doesn't treat
   Enter/Space as A — but more importantly it was solving the wrong problem.

The real tell was the user's own observation: *the keyboard stops working only when
it runs at the same time as the gamepad.* That is not an authentication story. It is
an **enumeration** story — something about presenting two interfaces at once was
malformed for the Switch 2's host stack.

## The real root cause

**The Switch 2's USB port runs at USB 3.0 SuperSpeed, and the device never declared
itself as a SuperSpeed device.**

[source/usb_composite.c](source/usb_composite.c) only set **Full Speed** and **High
Speed** device descriptors, with no **BOS** descriptor and no **SuperSpeed endpoint
companion** descriptors. A USB 3.0 host requires all three to bring a device up
correctly:

- a SuperSpeed device descriptor (`bcdUSB = 0x0300`, `bMaxPacketSize0 = 9`, i.e.
  2^9 = 512),
- a Binary Object Store (BOS) descriptor advertising the USB 2.0 + SuperSpeed
  capabilities,
- a SuperSpeed companion descriptor after every endpoint.

Why this hid for so long:

- The **original Switch** enumerates at High Speed, so the missing SuperSpeed data
  never mattered.
- On the Switch 2, a **single-interface** device (keyboard-only like Plume USB, or
  gamepad-only like CivSwitch) survived the incomplete SuperSpeed enumeration well
  enough to work. A **two-interface composite** did not — the configuration never
  came up usably, so neither interface delivered input. That is exactly the
  "only breaks when both run together" behavior.

## How RecurCIV revealed it

RecurCIV is built on the same Plume USB + CivSwitch base and does the same job. Its
USB bring-up in `source/usb_keyboard.c` uses the identical VID/PID
(`0x0F0D:0x0092`), identical strings, and the identical gamepad report descriptor —
but adds precisely the SuperSpeed pieces this project was missing:

```c
dev.bcdUSB = 0x0300; dev.bMaxPacketSize0 = 9;
usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev);
const uint8_t bos[] = {5,15,22,0,2, 7,16,2,2,0,0,0, 10,16,3,0,14,0,1,10,0xff,7};
usbDsSetBinaryObjectStore(bos, sizeof(bos));
```

It also appends config data across three speeds (Full/High/Super) with a companion
descriptor on each endpoint at SuperSpeed, uses 1 ms polling intervals, and keeps
the keyboard **IN-only** (one endpoint) with LEDs handled over EP0.

## What changed

All edits are in [source/usb_composite.c](source/usb_composite.c). Nothing about the
device identity, the report descriptors, the macro engine, or the web/UI layers was
touched.

1. **SuperSpeed device descriptor + BOS descriptor** added after the Full/High Speed
   descriptors in `usb_composite_init()`.

2. **Three-speed configuration loop with SuperSpeed companions.** `append_interface`
   now iterates Full/High/Super and appends a `{6,48,0,0,size,0}` companion after
   each endpoint at SuperSpeed. Polling interval is 1 frame at Full Speed and
   `bInterval = 4` (1 ms) at High/Super.

3. **Keyboard is now IN-only.** The redundant keyboard OUT endpoint was removed —
   LEDs already arrive via the EP0 `SET_REPORT` path that the control handler
   implements. Endpoint addresses now follow usb:ds's natural allocation
   (gamepad IN `0x81`, gamepad OUT `0x01`, keyboard IN `0x82`), and the old
   "dummy endpoint" address-bumping hacks were deleted. Interfaces are registered,
   then all config data appended, then endpoints registered in that order — matching
   how RecurCIV gets the addresses to line up with the declared descriptors.

### Endpoint layout, before and after

| | Before | After (matches RecurCIV) |
|---|---|---|
| Device speeds declared | Full, High | Full, High, **SuperSpeed** |
| BOS descriptor | none | present |
| SS endpoint companions | none | on every endpoint |
| Keyboard interface | IN + OUT (`0x83` / `0x03`) | **IN-only** (`0x82`) |
| Gamepad OUT endpoint | `0x02` (+ dummy `0x01`) | `0x01` |
| HS/SS poll interval | 1 | 4 (1 ms) |

## Verification so far

- The edited file compiles cleanly against the installed devkitPro toolchain and
  libnx (`aarch64-none-elf-gcc -Wall -Wextra`, zero warnings). libnx provides the
  `usbDsSetBinaryObjectStore` and `UsbDeviceSpeed_Super` APIs used.
- A full `.nro` link was **not** produced in this session — the shell used had no
  `make`, so the Makefile and the libmicrohttpd dependency build could not run.
  Build it in a devkitPro MSYS2 shell with `make`.

## Still to confirm on hardware

The fix is implemented and compiles; it has **not** yet been verified on a Switch 2.
After flashing:

- Re-check `/api/debug`: `pad_sent` / `kbd_sent` should climb with `reports_failed:0`
  **and** the console should actually register the A presses and keystrokes.
- `/api/log?which=usb` should show the new SuperSpeed / BOS init steps and a
  configured device.

Caveat worth keeping in mind: RecurCIV's own README lists native Switch 2 execution
as "untested," and its Switch 2 notes only confirm the **A** button mapping (which is
all this project's macro navigation relies on). That's the tested path, but the rest
is unverified until a real run.
