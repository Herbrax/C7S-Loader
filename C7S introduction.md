# C7S Introduction

A `.C7S` file is a small text script that C7S-Loader types into the other console over USB HID. It is not JavaScript and it is not a binary — it is a sequence of keyboard and/or gamepad actions. Everything is sent by the Switch 1 running C7S-Loader to the target console.

There are three kinds of `.C7S`, detected automatically:

1. **Line mode** — every line is typed as text, followed by Enter. No gamepad input. Good for plain text tests, SmileBASIC programs, etc.
2. **Civ 7 chat mode** — the CivSwitch chat-XSS sequence (A×5 preamble, Esc, gamepad A, type the line, Enter×2). Used by the 87-line `CIV7-RemoteJSLoader.C7S`.
3. **Script mode** — explicit timed keyboard/gamepad events written as `@wait`, `@key`, and `@pad` lines. This is what **Record presses** produces.

## File format

```text
Title - <line count>
---BEGIN---
...body...
---END---
```

- The first line is `<title> - <number>`. The number must match the number of body lines or the app shows a warning (it still runs).
- `---BEGIN---` must be the second line.
- The body follows, one action or text line per line. Blank lines are preserved as empty typed lines.
- `---END---` closes the file.
- Encoding is UTF-8 (a BOM is tolerated); CRLF and LF both work.
- Maximum file size is 1 MB.

### Mode detection

| File looks like | Mode |
|---|---|
| First body line starts with `@` | Script mode |
| Title contains `Remote JS`, or first body line contains `onerror` / `p=window.p` | Civ 7 chat mode |
| Anything else | Line mode |

## Line mode

Each non-empty line is typed exactly as written and then Enter is pressed.

```text
Test Payload - 2
---BEGIN---
hello from C7S-Loader
second line works
---END---
```

To type a line that starts with `@`, it is currently interpreted as script mode — do not start line-mode text with `@`.

## Script mode (`@` directives)

Script mode is state-based. A state is the current keyboard report plus the current gamepad report. The most recent state stays active until another line changes it.

### `@wait <milliseconds>`

Holds the **current** state for that many milliseconds, then the next line takes effect.

### `@key <modifier> <key>`

Sets the keyboard state. `@key 0 0` releases all keys.

Modifier bits (add them together):

| Modifier | Value |
|---|---|
| Left Ctrl | 1 |
| Left Shift | 2 |
| Left Alt | 4 |
| Left GUI | 8 |

Common HID key values:

| Key | Value | Key | Value |
|---|---|---|---|
| A…Z | 4–29 | 1…9 | 30–38 |
| 0 | 39 | Enter | 40 |
| Esc | 41 | Backspace | 42 |
| Tab | 43 | Space | 44 |
| `-` | 45 | `=` | 46 |
| `[` | 47 | `]` | 48 |
| `\` | 49 | `;` | 51 |
| `'` | 52 | `` ` `` | 53 |
| `,` | 54 | `.` | 55 |
| `/` | 56 | Right / Left / Down / Up | 79 / 80 / 81 / 82 |

Example: `@key 4 40` = Alt+Enter.

### `@pad <buttons> <hat> <lx> <ly> <rx> <ry>`

Sets the gamepad state.

**Buttons** (add them together):

| Button | Value | Button | Value |
|---|---|---|---|
| Y | 1 | B | 2 |
| A | 4 | X | 8 |
| L | 16 | R | 32 |
| ZL | 64 | ZR | 128 |
| Minus | 256 | Plus | 512 |
| L3 | 1024 | R3 | 2048 |
| Home | 4096 | Capture | 8192 |

**Hat** (D-pad):

| Direction | Value |
|---|---|
| Up | 0 |
| Up-right | 7 |
| Right | 6 |
| Down-right | 5 |
| Down | 4 |
| Down-left | 3 |
| Left | 2 |
| Up-left | 1 |
| Neutral | 8 |

**Sticks**: 0–255, centre is 128.

A neutral pad is:

```text
@pad 0 8 128 128 128 128
```

A quick A tap:

```text
@pad 4 8 128 128 128 128
@wait 80
@pad 0 8 128 128 128 128
```

### Full script example

```text
My Macro - 8
---BEGIN---
@wait 500
@key 0 4
@wait 80
@key 0 0
@wait 500
@pad 4 8 128 128 128 128
@wait 80
@pad 0 8 128 128 128 128
---END---
```

`CIV7-Reach-Chat-Input.C7S` is a real script-mode recording.

## Recording with the app

1. Open **C7S-Loader** on the Switch 1.
2. On the home screen, tap **Live input**.
3. Tap **Carry joycons controls** if you want the Switch 1's physical controller to be sent to the Switch 2. The app stops using that controller for its own menus while this is on; use the touchscreen for the app UI.
4. Tap **Record presses**. It changes to **Stop recording**.
5. Do your keyboard (on-screen or web Live input) and/or carried gamepad actions.
6. Tap **Stop recording**.
7. The app writes `sdmc:/switch/C7S-Loader/Recorded-<timestamp>.C7S`.
8. Download it from the web app with the **Download** link next to the file, or via FTP from `/switch/C7S-Loader/`.

The recorder writes explicit `@wait` / `@key` / `@pad` states, so the result is replayable by the app.

## Playing a C7S

1. Put the file in `sdmc:/switch/C7S-Loader/` (web upload, FTP, or copy to the SD card).
2. Select it in the app.
3. Press **START** (or A → START).
4. The app counts down 5 seconds. Focus the target input (Civ 7 chat, SmileBASIC editor, etc.).
5. Cancel with B or the on-screen CANCEL button. Hold **+** for 2 seconds to quit the app.

## Preloaded files

The NRO embeds two files and writes them to `sdmc:/switch/C7S-Loader/` on first launch **only if they are missing**:

- `CIV7-RemoteJSLoader.C7S` — the 87-line CivSwitch chat payload.
- `CIV7-Reach-Chat-Input.C7S` — the recorded input script.

Deleting a preloaded file and restarting the app restores it.

## Tips

- Line/script mode is one key per report; the app waits for each transfer.
- `@wait` is the time the **previous** state is held. Keep the first `@wait 0`/initial state at the top of a recording.
- For Civ 7, the 87-line payload is one line per chat message; do not reformat or reflow those lines.
- Use US English for the target console's USB keyboard layout.
