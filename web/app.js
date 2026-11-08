const $ = (id) => document.getElementById(id);

let files = [];
let selected = null;
let selPreviewToken = 0;
let lastState = null;
let liveOn = false;
let liveRunning = false;
let liveMods = 0;
let livePad = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };
const LIVE_HAT_NONE = 8;

/* ---------- helpers ---------- */
function fmtSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(1) + " MB";
}
function setMsg(el, text, kind) {
  if (!el) return;
  el.textContent = text || "";
  el.className = "msg" + (kind ? " " + kind : "");
}
async function jsonFetch(url, opts) {
  const r = await fetch(url, opts);
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
  if (!r.ok) throw new Error((data && data.error) || ("HTTP " + r.status));
  if (!data) throw new Error("empty response");
  return data;
}

/* ---------- mode tags ----------
   The API exposes `plain` (text mode) and `title`, not the raw body, so script
   vs Civ 7 chat is derived from the title the way the loader names them. */
function modeOf(f) {
  if (f.error) return { key: "unknown", label: "UNREADABLE", cls: "b-unknown" };
  if (f.plain) return { key: "text", label: "TEXT", cls: "b-text" };
  // Only the RemoteJS payload is true Civ 7 chat mode; other non-plain files are
  // recorded @-scripts. The API doesn't expose the body, so match on its name/title.
  const t = (f.title || "") + " " + (f.name || "");
  if (/remote\s*js/i.test(t)) return { key: "chat", label: "CIV 7 CHAT", cls: "b-chat" };
  return { key: "script", label: "SCRIPT", cls: "b-script" };
}
function modeIcon(key) {
  if (key === "text") return '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#8fc1ff" stroke-width="2.2" stroke-linecap="round"><path d="M5 6h14M12 6v13M8 19h8"/></svg>';
  if (key === "chat") return '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#ecd291" stroke-width="2.2" stroke-linejoin="round"><path d="M4 5h16v11H9l-5 4z"/></svg>';
  if (key === "unknown") return '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#ffb0b0" stroke-width="2.2" stroke-linecap="round"><path d="M12 3 2 20h20zM12 10v4M12 17v.5"/></svg>';
  return '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#7ee6b8" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 5 3 12l5 7M16 5l5 7-5 7"/></svg>';
}
function modeExplain(key) {
  if (key === "text") return "Types each line, then presses Enter.";
  if (key === "chat") return "Opens Civ 7 chat with the gamepad, types each line, and sends it.";
  if (key === "unknown") return "This file could not be parsed as a .C7S script.";
  return "Replays the recorded key and button timings.";
}
const iconBg = { text: "rgba(47,140,255,.14)", chat: "rgba(217,182,94,.14)", script: "rgba(60,207,145,.13)", unknown: "rgba(255,107,107,.13)" };
const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");

/* ---------- files ---------- */
function renderFiles() {
  const box = $("files");
  box.textContent = "";
  $("fileCount").textContent = files.length + (files.length === 1 ? " file" : " files");
  if (!files.length) {
    box.innerHTML = '<div class="empty"><svg width="56" height="56" viewBox="0 0 24 24" fill="none" stroke="#5d6a86" stroke-width="1.4" stroke-linejoin="round"><path d="M6 2.5h8l4.5 4.5v14.5H6z"/><path d="M14 2.5V7h4.5M9 13h6M9 16.5h4"/></svg><div class="big">No scripts yet</div><div>Drop a .C7S above, or record one from the console.</div></div>';
    return;
  }
  for (const f of files) {
    const m = modeOf(f);
    const row = document.createElement("div");
    row.className = "file" + (f.name === selected ? " active" : "");
    const meta = f.header
      ? `${f.title} · ${f.lines} lines`
      : (f.error ? "unreadable file" : `${f.lines} lines · no header`);
    row.innerHTML =
      `<div class="fi" style="background:${iconBg[m.key]}">${modeIcon(m.key)}</div>` +
      `<div class="fmid"><div class="nm">${esc(f.name)}</div><div class="mt">${esc(meta)}</div></div>` +
      `<div class="right"><div class="sz"><span class="badge ${m.cls}">${m.label}</span> ${fmtSize(f.size)}</div>` +
      `<a class="dl" href="/api/download?name=${encodeURIComponent(f.name)}" download="${esc(f.name)}">Download</a></div>`;
    row.querySelector(".dl").addEventListener("click", (e) => e.stopPropagation());
    row.addEventListener("click", () => selectFile(f.name));
    box.appendChild(row);
  }
}

async function refreshFiles() {
  try {
    files = await jsonFetch("/api/files");
    files.sort((a, b) => a.name.localeCompare(b.name));
    renderFiles();
    if (selected && !files.some((f) => f.name === selected)) { selected = null; selectFile(null); }
  } catch (e) {
    $("files").innerHTML = `<p class="msg err">Cannot list files: ${esc(e.message)}</p>`;
  }
}

async function loadPreview(name, token) {
  const el = $("selPreview");
  el.textContent = "Loading preview…";
  try {
    const r = await fetch("/api/download?name=" + encodeURIComponent(name));
    const text = await r.text();
    if (token !== selPreviewToken) return;
    let lines = text.split(/\r?\n/);
    const b = lines.findIndex((l) => l.trim() === "---BEGIN---");
    if (b >= 0) lines = lines.slice(b + 1);
    lines = lines.filter((l) => l.trim() !== "---END---").slice(0, 8);
    el.innerHTML = lines.map((l, i) => `<span class="ln">${i + 1}</span>${esc(l)}`).join("\n") || '<span class="muted">(empty)</span>';
  } catch (e) {
    if (token === selPreviewToken) el.textContent = "Preview unavailable.";
  }
}

function selectFile(name) {
  selected = name;
  renderFiles();
  const f = files.find((x) => x.name === name);
  if (!f) { $("selEmpty").classList.remove("hidden"); $("selBody").classList.add("hidden"); return; }
  $("selEmpty").classList.add("hidden");
  $("selBody").classList.remove("hidden");
  const m = modeOf(f);
  $("selBadge").innerHTML = `<span class="badge ${m.cls}">${m.label}</span>`;
  $("selName").textContent = f.name;
  const facts = [`<span><b>${f.lines}</b> lines</span>`, `<span><b>${fmtSize(f.size)}</b></span>`];
  if (f.header && f.title) facts.push(`<span>"${esc(f.title)}"</span>`);
  $("selFacts").innerHTML = facts.join("");
  const warn = $("selWarn");
  if (f.mismatch) { warn.classList.remove("hidden"); warn.textContent = `Header says ${f.declared} lines but the file has ${f.lines}. It will still run.`; }
  else warn.classList.add("hidden");
  $("selExplain").textContent = modeExplain(m.key) + " A 5-second countdown gives you time to focus the target field on the Switch 2.";
  setMsg($("startMsg"), "", "");
  selPreviewToken++;
  loadPreview(f.name, selPreviewToken);
}

/* ---------- upload ---------- */
async function upload(file) {
  if (!file) return;
  if (!/\.c7s$/i.test(file.name)) { setMsg($("uploadMsg"), "Only .C7S files are accepted.", "err"); return; }
  setMsg($("uploadMsg"), "Uploading " + file.name + "…", "");
  try {
    const data = await jsonFetch("/api/upload?name=" + encodeURIComponent(file.name), { method: "POST", body: file });
    setMsg($("uploadMsg"), "Uploaded " + data.name + ".", "ok");
    await refreshFiles();
    selectFile(data.name);
  } catch (e) {
    setMsg($("uploadMsg"), "Upload failed: " + e.message, "err");
  }
}

/* ---------- send / cancel ---------- */
async function start() {
  if (!selected) return;
  $("startBtn").disabled = true;
  setMsg($("startMsg"), "Starting…", "");
  try {
    await jsonFetch("/api/start?name=" + encodeURIComponent(selected), { method: "POST" });
    setMsg($("startMsg"), "Starting in 5s — focus the target field on the Switch 2.", "ok");
  } catch (e) {
    setMsg($("startMsg"), "Cannot start: " + e.message, "err");
  }
  $("startBtn").disabled = false;
  poll();
}
async function cancel() {
  $("cancelBtn").disabled = true;
  try { await jsonFetch("/api/cancel", { method: "POST" }); } catch (e) {}
  $("cancelBtn").disabled = false;
  poll();
}

/* ---------- state / polling ---------- */
function stateName(n) {
  return ({ 0: "reset", 1: "suspended", 2: "wait powered", 3: "wait default", 4: "address", 5: "Configured" })[n] || ("state " + n);
}
function updateLinkChip(u) {
  const dot = $("linkDot"), txt = $("linkText");
  if (!u || !u.initialized) { dot.className = "dot err"; txt.textContent = "Plug in USB"; }
  else if (u.pad_ready && u.kbd_ready) { dot.className = "dot ok"; txt.textContent = "Switch 2 linked"; }
  else { dot.className = "dot warn"; txt.textContent = "USB waiting"; }
}
function updateStatusTab(s) {
  const u = s.usb || {};
  const linked = u.initialized && u.pad_ready && u.kbd_ready;
  const orb = $("usbOrb");
  if (linked) {
    orb.style.background = "rgba(60,207,145,.16)";
    orb.innerHTML = '<svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="#3ccf91" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><path d="m5 12.5 4.5 4.5L19 7.5"/></svg>';
    $("usbTitle").textContent = "Linked";
    $("usbSub").textContent = "Ready to send keys and buttons";
  } else {
    orb.style.background = "rgba(255,196,64,.16)";
    orb.innerHTML = '<svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="#ffc440" stroke-width="2.6" stroke-linecap="round"><path d="M12 3 2 20h20zM12 10v4M12 17v.5"/></svg>';
    $("usbTitle").textContent = u.initialized ? "Waiting for the Switch 2" : "Not started";
    $("usbSub").textContent = u.initialized ? "" : (u.stage || "check USB log");
  }
  const rdy = (b) => b ? '<span class="ok">Ready</span>' : '<span class="warn">Waiting</span>';
  $("usbPad").innerHTML = rdy(u.pad_ready);
  $("usbKbd").innerHTML = rdy(u.kbd_ready);
  $("usbState").textContent = stateName(u.state);
  $("usbFailed").textContent = u.reports_failed != null ? u.reports_failed : "—";
  const host = s.ip && s.ip !== "unknown" ? s.ip : location.hostname;
  $("serverUrl").textContent = "http://" + host + ":" + s.port;
}
function updateState(s) {
  const prev = lastState;
  lastState = s;
  updateLinkChip(s.usb);
  updateStatusTab(s);
  if (document.activeElement !== $("port")) $("port").value = s.port;

  liveRunning = !!s.running;
  if (liveRunning && !(prev && prev.running)) resetLiveState();
  setLiveControls();

  const running = !!s.running;
  $("runbar").classList.toggle("hidden", !running);
  if (running) {
    const pct = s.total > 0 ? Math.min(100, Math.round((s.step / s.total) * 100)) : 0;
    $("runPct").textContent = pct + "%";
    $("runFill").style.width = pct + "%";
    $("runMeta").textContent = (s.selected || "") + (s.total > 0 ? " · step " + s.step + " / " + s.total : " · starting…");
  }
  if (s.finished && !s.running && prev && prev.running) {
    setMsg($("startMsg"), (s.ok ? "Sent. " : "Stopped. ") + (s.message || ""), s.ok ? "ok" : "err");
  }
}
async function poll() {
  try { updateState(await jsonFetch("/api/state")); }
  catch (e) { $("linkDot").className = "dot err"; $("linkText").textContent = "offline"; }
}

/* ---------- logs ---------- */
function setConsole(id, text) {
  const el = $(id);
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
  el.textContent = text;
  if (atBottom) el.scrollTop = el.scrollHeight;
}
async function refreshDebug() {
  try {
    const [ul, rl] = await Promise.all([jsonFetch("/api/log?which=usb"), jsonFetch("/api/log?which=run")]);
    setConsole("usbConsole", (ul && ul.text) || "");
    setConsole("runConsole", (rl && rl.text) || "");
  } catch (e) {}
}

/* ---------- settings / update / ktest ---------- */
async function savePort() {
  const port = parseInt($("port").value, 10);
  if (!port || port < 1 || port > 65535) { setMsg($("settingsMsg"), "Enter a port between 1 and 65535.", "err"); return; }
  const oldPort = lastState ? lastState.port : port;
  setMsg($("settingsMsg"), "Saving and restarting…", "");
  try {
    await jsonFetch("/api/settings?port=" + port, { method: "POST" });
    setMsg($("settingsMsg"), "Restarting on port " + port + ". Reconnecting…", "ok");
    setTimeout(() => { location.href = "http://" + location.hostname + ":" + port + "/"; }, 1800);
  } catch (e) {
    setMsg($("settingsMsg"), "Cannot save: " + e.message, "err");
    $("port").value = oldPort;
  }
}
async function keyboardTest() {
  const btn = $("ktestBtn");
  btn.disabled = true;
  const label = btn.textContent;
  btn.textContent = "Starting…";
  setMsg($("ktestMsg"), "Typing \"hello\" in 5s — focus a text field on the Switch 2.", "");
  try { await jsonFetch("/api/ktest", { method: "POST" }); }
  catch (e) { setMsg($("ktestMsg"), "Cannot run: " + e.message, "err"); }
  setTimeout(() => { btn.disabled = false; btn.textContent = label; }, 6000);
}
async function uploadUpdate() {
  const f = $("nroFile").files[0];
  if (!f) { setMsg($("nroMsg"), "Choose a .nro first.", "err"); return; }
  if (!/\.nro$/i.test(f.name)) { setMsg($("nroMsg"), "Only .nro files are accepted.", "err"); return; }
  $("nroUpload").disabled = true;
  setMsg($("nroMsg"), "Uploading " + f.name + " (" + fmtSize(f.size) + ")…", "");
  try {
    const r = await fetch("/api/update", { method: "POST", body: f });
    const d = await r.json().catch(() => ({}));
    if (!r.ok || !d.ok) throw new Error(d.error || ("HTTP " + r.status));
    setMsg($("nroMsg"), "Update uploaded. The app will close; reopen it from hbmenu.", "ok");
  } catch (e) {
    setMsg($("nroMsg"), "Update failed: " + e.message, "err");
  }
  $("nroUpload").disabled = false;
}

/* ---------- live input ---------- */
function neutralPad() { return { buttons: 0, hat: LIVE_HAT_NONE, lx: 128, ly: 128, rx: 128, ry: 128 }; }
function liveSetKey(mod, key) { if (!liveOn || liveRunning) return; liveFetch("/api/live/key?mod=" + mod + "&key=" + key); }
function sendPad() {
  if (!liveOn || liveRunning) return;
  const p = livePad;
  liveFetch("/api/live/pad?buttons=" + p.buttons + "&hat=" + p.hat + "&lx=" + p.lx + "&ly=" + p.ly + "&rx=" + p.rx + "&ry=" + p.ry);
}
async function liveFetch(path) {
  if (!liveOn || liveRunning) return;
  try { await jsonFetch(path, { method: "POST" }); }
  catch (e) { setMsg($("liveMsg"), "Live input: " + e.message, "err"); }
}
function setLiveControls() {
  const disabled = !liveOn || liveRunning;
  for (const el of document.querySelectorAll(".liveControl")) el.disabled = disabled;
  $("liveSw").classList.toggle("on", liveOn);
  $("liveState").textContent = liveRunning ? "paused (sending)" : (liveOn ? "on" : "off");
}
function resetLiveState() {
  liveMods = 0;
  livePad = neutralPad();
  for (const el of document.querySelectorAll(".liveControl.active")) el.classList.remove("active");
}
function releaseAllLive() {
  resetLiveState();
  if (liveOn && !liveRunning) liveFetch("/api/live?release=1");
  setLiveControls();
}
async function enableLive(on) {
  try {
    const d = await jsonFetch("/api/live?on=" + (on ? 1 : 0), { method: "POST" });
    liveOn = !!d.enabled;
    setMsg($("liveMsg"), liveOn ? "Live input enabled." : "Live input off.", liveOn ? "ok" : "");
  } catch (e) {
    liveOn = false;
    setMsg($("liveMsg"), "Cannot enable live input: " + e.message, "err");
  }
  if (!liveOn) resetLiveState();
  setLiveControls();
}
function bindKeyButton(btn, key) {
  btn.addEventListener("pointerdown", (e) => {
    if (!liveOn || liveRunning) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    btn.classList.add("active");
    liveSetKey(liveMods, key);
  });
  const up = () => { if (!btn.classList.contains("active")) return; btn.classList.remove("active"); liveSetKey(liveMods, 0); };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}
function bindShortcut(btn, mod, key) {
  let down = false, started = 0;
  btn.addEventListener("pointerdown", (e) => {
    if (!liveOn || liveRunning || down) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    down = true; started = performance.now();
    btn.classList.add("active");
    liveSetKey(mod, key);
  });
  const up = () => {
    if (!down) return;
    down = false;
    const wait = Math.max(0, 80 - (performance.now() - started));
    setTimeout(() => { btn.classList.remove("active"); liveSetKey(liveMods, 0); }, wait);
  };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}
function bindPadButton(btn, bit) {
  let down = false;
  btn.addEventListener("pointerdown", (e) => {
    if (!liveOn || liveRunning || down) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    down = true; btn.classList.add("active");
    livePad.buttons |= bit; sendPad();
  });
  const up = () => { if (!down) return; down = false; btn.classList.remove("active"); livePad.buttons &= ~bit; sendPad(); };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}
function bindHatButton(btn, hat) {
  let down = false;
  btn.addEventListener("pointerdown", (e) => {
    if (!liveOn || liveRunning || down) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    down = true; btn.classList.add("active");
    livePad.hat = hat; sendPad();
  });
  const up = () => { if (!down) return; down = false; btn.classList.remove("active"); livePad.hat = LIVE_HAT_NONE; sendPad(); };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}
function padButton(label, bit, extra) {
  const b = document.createElement("button");
  b.type = "button";
  b.className = "liveControl pb" + (extra ? " " + extra : "");
  b.textContent = label;
  bindPadButton(b, bit);
  return b;
}
function initLive() {
  const rows = [
    [{ l: "`", k: 53 }, { l: "1", k: 30 }, { l: "2", k: 31 }, { l: "3", k: 32 }, { l: "4", k: 33 }, { l: "5", k: 34 }, { l: "6", k: 35 }, { l: "7", k: 36 }, { l: "8", k: 37 }, { l: "9", k: 38 }, { l: "0", k: 39 }, { l: "-", k: 45 }, { l: "=", k: 46 }, { l: "⌫", k: 42, w: "fn w15" }],
    [{ l: "Tab", k: 43, w: "fn w15" }, { l: "q", k: 20 }, { l: "w", k: 26 }, { l: "e", k: 8 }, { l: "r", k: 21 }, { l: "t", k: 23 }, { l: "y", k: 28 }, { l: "u", k: 24 }, { l: "i", k: 12 }, { l: "o", k: 18 }, { l: "p", k: 19 }, { l: "[", k: 47 }, { l: "]", k: 48 }, { l: "\\", k: 49, w: "w15" }],
    [{ l: "Esc", k: 41, w: "fn w175" }, { l: "a", k: 4 }, { l: "s", k: 22 }, { l: "d", k: 7 }, { l: "f", k: 9 }, { l: "g", k: 10 }, { l: "h", k: 11 }, { l: "j", k: 13 }, { l: "k", k: 14 }, { l: "l", k: 15 }, { l: ";", k: 51 }, { l: "'", k: 52 }, { l: "Enter", k: 40, w: "fn w225" }],
    [{ l: "z", k: 29 }, { l: "x", k: 27 }, { l: "c", k: 6 }, { l: "v", k: 25 }, { l: "b", k: 5 }, { l: "n", k: 17 }, { l: "m", k: 16 }, { l: ",", k: 54 }, { l: ".", k: 55 }, { l: "/", k: 56 }, { l: "↑", k: 82 }, { l: "←", k: 80 }, { l: "↓", k: 81 }, { l: "→", k: 79 }],
    [{ l: "Space", k: 44, w: "fn w8" }],
  ];
  const kb = $("liveKeyboard");
  for (const row of rows) {
    const r = document.createElement("div");
    r.className = "krow";
    for (const item of row) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "liveControl k" + (item.w ? " " + item.w : "");
      b.textContent = item.l;
      if (item.noop) { b.disabled = true; b.style.opacity = ".55"; }
      else bindKeyButton(b, item.k);
      r.appendChild(b);
    }
    kb.appendChild(r);
  }
  for (const b of document.querySelectorAll(".mod")) {
    b.addEventListener("click", () => {
      if (!liveOn || liveRunning) return;
      const bit = parseInt(b.dataset.mod, 10) || 0;
      liveMods ^= bit;
      b.classList.toggle("active", !!(liveMods & bit));
      liveSetKey(liveMods, 0);
    });
  }
  for (const b of document.querySelectorAll(".shortcut")) {
    const p = b.dataset.shortcut.split(",").map(Number);
    bindShortcut(b, p[0], p[1]);
  }
  const dpad = [["▲", 0, "up"], ["◀", 2, "left"], ["▶", 6, "right"], ["▼", 4, "down"]];
  for (const [label, hat, cls] of dpad) {
    const b = document.createElement("button");
    b.type = "button"; b.className = "liveControl pb " + cls; b.textContent = label;
    bindHatButton(b, hat);
    $("liveDpad").appendChild(b);
  }
  const face = [["X", 8, "gx"], ["Y", 1, "gy"], ["A", 4, "ga"], ["B", 2, "gb"]];
  for (const [label, bit, cls] of face) $("liveFace").appendChild(padButton(label, bit, cls));
  for (const [label, bit] of [["L", 16], ["ZL", 64], ["R", 32], ["ZR", 128]]) $("liveShoulders").appendChild(padButton(label, bit));
  for (const [label, bit] of [["−", 256], ["+", 512], ["L3", 1024], ["R3", 2048], ["Home", 4096], ["Capture", 8192]]) $("liveSystem").appendChild(padButton(label, bit));

  $("liveToggle").addEventListener("click", () => { if (!liveRunning) enableLive(!liveOn); });
  $("liveRelease").addEventListener("click", releaseAllLive);
  window.addEventListener("beforeunload", () => {
    if (liveOn && !liveRunning) { try { navigator.sendBeacon("/api/live?release=1"); } catch (e) {} }
  });
  setLiveControls();
}
async function loadLive() {
  try { const d = await jsonFetch("/api/live"); liveOn = !!d.enabled; }
  catch (e) { liveOn = false; }
  if (liveOn) liveFetch("/api/live?release=1");
  setLiveControls();
}

/* ---------- tabs ---------- */
const TABS = ["send", "live", "status", "ldn"];
function switchTab(name) {
  if (!TABS.includes(name)) name = "send";
  if (name !== "live" && liveOn) enableLive(false);
  for (const b of document.querySelectorAll(".tabbtn")) b.classList.toggle("on", b.dataset.tab === name);
  for (const id of TABS) $("tab-" + id).classList.toggle("hidden", id !== name);
  if (location.hash.slice(1) !== name) history.replaceState(null, "", "#" + name);
}

/* ---------- wiring ---------- */
function wire() {
  for (const b of document.querySelectorAll(".tabbtn")) b.addEventListener("click", () => switchTab(b.dataset.tab));
  window.addEventListener("hashchange", () => switchTab(location.hash.slice(1)));
  $("browse").addEventListener("click", () => $("file").click());
  $("file").addEventListener("change", () => upload($("file").files[0]));
  $("refreshBtn").addEventListener("click", refreshFiles);
  $("startBtn").addEventListener("click", start);
  $("cancelBtn").addEventListener("click", cancel);
  $("savePort").addEventListener("click", savePort);
  $("port").addEventListener("keydown", (e) => { if (e.key === "Enter") savePort(); });
  $("ktestBtn").addEventListener("click", keyboardTest);
  $("ldnStart").addEventListener("click", startLdn);
  $("nroBrowse").addEventListener("click", () => $("nroFile").click());
  $("nroUpload").addEventListener("click", uploadUpdate);
  $("nroFile").addEventListener("change", () => {
    const f = $("nroFile").files[0];
    if (f) setMsg($("nroMsg"), f.name + " (" + fmtSize(f.size) + ")", "");
  });
  const drop = $("drop");
  for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
  drop.addEventListener("drop", (e) => upload(e.dataTransfer.files && e.dataTransfer.files[0]));
}

async function pollLdn() {
  try {
    const d = await jsonFetch("/api/ldn");
    $("ldnStatus").textContent = d.status || "idle";
    $("ldnLog").textContent = d.log || "";
    const b = $("ldnStart");
    if (b) { b.disabled = !!d.active; b.textContent = d.active ? "LDN running" : "Start LDN"; }
  } catch (e) {}
}

async function startLdn() {
  const b = $("ldnStart");
  if (b) { b.disabled = true; b.textContent = "Starting..."; }
  try { await jsonFetch("/api/ldn?start=1", { method: "POST" }); } catch (e) {}
  setTimeout(pollLdn, 300);
}

async function boot() {
  wire();
  initLive();
  await loadLive();
  await poll();
  await refreshFiles();
  if (lastState && lastState.selected && files.some((f) => f.name === lastState.selected)) selectFile(lastState.selected);
  if (location.hash) switchTab(location.hash.slice(1));
  setInterval(poll, 1000);
  refreshDebug();
  setInterval(refreshDebug, 2000);
  pollLdn();
  setInterval(pollLdn, 1000);
}
boot();
