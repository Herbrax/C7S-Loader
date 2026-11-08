# proto1 — UX redesign mockups

These are **visual mockups only**. No loader code was changed. The mockups are
HTML rendered to PNG with headless Chrome, using the app's real fonts
(`assets/fonts`) and the real 1920×1080 framebuffer size for the console shots.

- `src/console.html` — the on-TV UI (ImGui-drawn today). `?s=<screen>`.
- `src/web.html` — the browser UI served by the console. `?s=<view>`.
- `render.sh` — regenerates all PNGs.

Every label, mode, button, API and status value shown maps to something that
already exists in the code, so this can be built against the current backend.

## What changed and why

**One idea per screen, named for what it does.** The current build spreads
Payloads / Settings / Live / web-only features across modal screens and a long
scrolling web page with raw debug logs up top. The redesign uses three tabs that
match the three real jobs:

| Tab | Job | Backed by |
|---|---|---|
| **Send** | pick a `.C7S` and type it into the Switch 2 | `c7s_list`, `/api/start`, run state |
| **Live** | type / press buttons live, record, pass-through | `live_*`, `carry_*`, `record_*` |
| **Connect** / **Status** | web URL, USB health, port, update | `/api/state`, `/api/log`, `/api/settings`, `/api/update` |

**Plain language instead of jargon.** "Payloads" → **Scripts**; "START" →
**Send to Switch 2**; "Carry joycons controls" → **Pass through Joy-Con**;
"USB READY/WAITING/FAILED" → a single **Switch 2 linked / Plug in USB** badge.
"header not recognised / Civ 7 chat / line mode" → three colored mode tags
(**TEXT**, **SCRIPT**, **CIV 7 CHAT**) shown on every file.

**The scary moments are explicit.** The 5-second pre-send window is a real
countdown screen ("Switch to the Switch 2 now"); running shows a readable
progress card; first-run with no USB and no files gets a step-by-step empty
state instead of a dead list.

**Live input is one screen, not two half-built ones.** Full keyboard + full
gamepad with a live "Sent" echo line, Record and Pass-through as toggles, and a
clear warning banner while pass-through steals the physical controller.

## Screens

Console (TV): `console-01..09` — send, countdown, sending, sent, live keyboard
(+shift), gamepad with pass-through, connect, first-run.

Web: `web-01..03` desktop (send, live, status) and `web-04/05` the same on a
phone.

## Open questions for review

- Console tabs: switch with **L/R** (shown) — OK, or keep a home screen?
- "Connect" vs "Status" as the third tab name (console says Connect, web says Status).
- Progress is counted in **steps** (the backend unit), not lines — kept honest; fine?
