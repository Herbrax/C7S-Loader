const $ = (id) => document.getElementById(id);

let files = [];
let selected = null;
let lastState = null;
let pollTimer = null;
let debugTimer = null;
let finishedBadge = null;
let finishedTimer = null;
let liveOn = false;
let liveRunning = false;
let liveMods = 0;
let livePad = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };
const LIVE_HAT_NONE = 8;

function fmtSize(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(1) + " MB";
}

function setMsg(el, text, kind) {
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

function renderFiles() {
  const box = $("files");
  box.textContent = "";
  if (!files.length) {
    const p = document.createElement("p");
    p.className = "muted";
    p.textContent = "No .C7S files yet.";
    box.appendChild(p);
    return;
  }
  for (const f of files) {
    const row = document.createElement("div");
    row.className = "file" + (f.name === selected ? " active" : "");
    const left = document.createElement("div");
    const name = document.createElement("div");
    name.className = "name";
    name.textContent = f.name;
    if (finishedBadge && finishedBadge.name === f.name) {
      const b = document.createElement("span");
      b.className = "badge " + (finishedBadge.ok ? "ok" : "err");
      b.textContent = finishedBadge.ok ? "finished" : "stopped";
      name.appendChild(b);
    }
    const meta = document.createElement("div");
    meta.className = "meta";
    if (f.header) {
      meta.textContent = f.title + " - " + f.declared + " lines" +
        (f.mismatch ? " (file has " + f.lines + ")" : "") +
        (f.plain ? " - line mode" : " - Civ 7 chat");
    } else {
      meta.textContent = "header not recognised - " + f.lines + " lines" +
        (f.plain ? " - line mode" : "");
    }
    left.appendChild(name);
    left.appendChild(meta);
    const size = document.createElement("div");
    size.className = "size";
    size.textContent = fmtSize(f.size);
    const dl = document.createElement("a");
    dl.className = "download";
    dl.href = "/api/download?name=" + encodeURIComponent(f.name);
    dl.download = f.name;
    dl.textContent = "Download";
    dl.addEventListener("click", (ev) => ev.stopPropagation());
    const right = document.createElement("div");
    right.className = "fileRight";
    right.appendChild(size);
    right.appendChild(dl);
    row.appendChild(left);
    row.appendChild(right);
    row.addEventListener("click", () => selectFile(f.name));
    box.appendChild(row);
  }
}

async function refreshFiles() {
  try {
    files = await jsonFetch("/api/files");
    files.sort((a, b) => a.name.localeCompare(b.name));
    renderFiles();
  } catch (e) {
    const box = $("files");
    box.textContent = "";
    const p = document.createElement("p");
    p.className = "msg err";
    p.textContent = "Cannot list files: " + e.message;
    box.appendChild(p);
  }
}

function selectFile(name) {
  selected = name;
  const f = files.find((x) => x.name === name);
  renderFiles();
  if (!f) { $("confirmPanel").classList.add("hidden"); return; }

  $("confirmPanel").classList.remove("hidden");
  const parts = [];
  parts.push("Are you sure you want to load " + f.name + "?");
  if (f.header) {
    parts.push("Payload " + f.title + " - lines " + f.declared + ".");
    if (f.mismatch) parts.push("Warning: the file has " + f.lines + " payload lines.");
  } else {
    parts.push("Payload header not recognised (" + f.lines + " lines).");
  }
  parts.push(f.plain ? "Writing mode: plain keyboard lines (no gamepad)." : "Writing mode: Civ 7 chat (Esc + gamepad A).");
  parts.push("This will type the payload into the other console.");
  $("confirmText").textContent = parts.join(" ");
  setMsg($("startMsg"), "", "");
}

async function upload(file) {
  if (!file) return;
  if (!/\.c7s$/i.test(file.name)) {
    setMsg($("uploadMsg"), "Only .C7S files are accepted.", "err");
    return;
  }
  setMsg($("uploadMsg"), "Uploading " + file.name + "...", "");
  try {
    const q = "/api/upload?name=" + encodeURIComponent(file.name);
    const data = await jsonFetch(q, { method: "POST", body: file });
    setMsg($("uploadMsg"), "Uploaded " + data.name + ".", "ok");
    await refreshFiles();
    selectFile(data.name);
  } catch (e) {
    setMsg($("uploadMsg"), "Upload failed: " + e.message, "err");
  }
}


function setConsole(id, text) {
  const el = $(id);
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
  el.textContent = text;
  if (atBottom) el.scrollTop = el.scrollHeight;
}

async function refreshDebug() {
  try {
    const [ul, rl] = await Promise.all([
      jsonFetch("/api/log?which=usb"),
      jsonFetch("/api/log?which=run"),
    ]);
    setConsole("usbConsole", (ul && ul.text) || "");
    setConsole("runConsole", (rl && rl.text) || "");
  } catch (e) {
  }
}

function setLiveDebug(on) {
  if (debugTimer) {
    clearInterval(debugTimer);
    debugTimer = null;
  }
  if (on) {
    refreshDebug();
    debugTimer = setInterval(refreshDebug, 1000);
  }
}

async function keyboardTest() {
  const btn = $("ktestBtn");
  btn.disabled = true;
  btn.textContent = "Starting...";
  try {
    await jsonFetch("/api/ktest", { method: "POST" });
  } catch (e) {
  }
  setTimeout(() => {
    btn.disabled = false;
    btn.textContent = "Test keys (5s)";
  }, 6000);
}


async function start() {
  if (!selected) return;
  $("startBtn").disabled = true;
  setMsg($("startMsg"), "Starting...", "");
  try {
    await jsonFetch("/api/start?name=" + encodeURIComponent(selected), { method: "POST" });
    setMsg($("startMsg"), "Starting in 5s - focus the target text field.", "ok");
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

function updateState(s) {
  const prev = lastState;
  lastState = s;
  $("server").textContent = (s.ip && s.ip !== "unknown" ? s.ip : location.hostname) + ":" + s.port;
  if (document.activeElement !== $("port")) $("port").value = s.port;

  liveRunning = !!s.running;
  if (liveRunning && !(prev && prev.running)) resetLiveState();
  setLiveControls();

  const running = !!s.running;

  $("runPanel").classList.toggle("hidden", !running);
  if (running) {
    $("runName").textContent = s.selected || "";
    const pct = s.total > 0 ? Math.min(100, Math.round((s.step / s.total) * 100)) : 0;
    $("barFill").style.width = pct + "%";
    $("runStep").textContent = s.total > 0 ? (s.step + " / " + s.total + " steps") : "starting...";
    $("confirmPanel").classList.add("hidden");
  }

  if (s.finished && !s.running && prev && prev.running) {
    showFinishedBadge(s.selected, s.ok);
  }
}

function showFinishedBadge(name, ok) {
  if (!name) return;
  finishedBadge = { name, ok };
  renderFiles();
  clearTimeout(finishedTimer);
  finishedTimer = setTimeout(() => {
    finishedBadge = null;
    renderFiles();
  }, 3000);
}

async function poll() {
  try {
    updateState(await jsonFetch("/api/state"));
  } catch (e) {
    $("server").textContent = "offline";
  }
}

async function uploadUpdate() {
  const f = $("nroFile").files[0];
  if (!f) {
    setMsg($("nroMsg"), "Choose a .nro first.", "err");
    return;
  }
  if (!/\.nro$/i.test(f.name)) {
    setMsg($("nroMsg"), "Only .nro files are accepted.", "err");
    return;
  }
  $("nroUpload").disabled = true;
  setMsg($("nroMsg"), "Uploading " + f.name + " (" + fmtSize(f.size) + ")...", "");
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

async function savePort() {
  const port = parseInt($("port").value, 10);
  if (!port || port < 1 || port > 65535) {
    setMsg($("settingsMsg"), "Enter a port between 1 and 65535.", "err");
    return;
  }
  const oldPort = lastState ? lastState.port : port;
  setMsg($("settingsMsg"), "Saving and restarting...", "");
  try {
    await jsonFetch("/api/settings?port=" + port, { method: "POST" });
    setMsg($("settingsMsg"), "Restarting on port " + port + ". Reconnecting...", "ok");
    setTimeout(() => {
      const proto = location.protocol === "https:" ? "https:" : "http:";
      location.href = proto + "//" + location.hostname + ":" + port + "/";
    }, 1800);
  } catch (e) {
    setMsg($("settingsMsg"), "Cannot save: " + e.message, "err");
    $("port").value = oldPort;
  }
}

function neutralPad() {
  return { buttons: 0, hat: LIVE_HAT_NONE, lx: 128, ly: 128, rx: 128, ry: 128 };
}

function liveSetKey(mod, key) {
  if (!liveOn || liveRunning) return;
  liveFetch("/api/live/key?mod=" + mod + "&key=" + key);
}

function sendPad() {
  if (!liveOn || liveRunning) return;
  const p = livePad;
  liveFetch("/api/live/pad?buttons=" + p.buttons + "&hat=" + p.hat +
    "&lx=" + p.lx + "&ly=" + p.ly + "&rx=" + p.rx + "&ry=" + p.ry);
}

async function liveFetch(path) {
  if (!liveOn || liveRunning) return;
  try {
    await jsonFetch(path, { method: "POST" });
  } catch (e) {
    setMsg($("liveMsg"), "Live input: " + e.message, "err");
  }
}

function setLiveControls() {
  const disabled = !liveOn || liveRunning;
  for (const el of document.querySelectorAll(".liveControl")) el.disabled = disabled;
  const en = $("liveEnable");
  if (en) en.disabled = liveRunning;
  const st = $("liveState");
  if (st) st.textContent = liveRunning ? "paused" : (liveOn ? "on" : "off");
}

function updateModButtons() {
  for (const b of document.querySelectorAll(".mod")) {
    const bit = parseInt(b.dataset.mod, 10) || 0;
    b.classList.toggle("active", !!(liveMods & bit));
  }
}

function resetLiveState() {
  liveMods = 0;
  livePad = neutralPad();
  for (const el of document.querySelectorAll(".liveControl.active")) el.classList.remove("active");
  updateModButtons();
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
  $("liveEnable").checked = liveOn;
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
  const up = () => {
    if (!btn.classList.contains("active")) return;
    btn.classList.remove("active");
    liveSetKey(liveMods, 0);
  };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}

function bindShortcut(btn, mod, key) {
  let down = false;
  let started = 0;
  btn.addEventListener("pointerdown", (e) => {
    if (!liveOn || liveRunning || down) return;
    e.preventDefault();
    try { btn.setPointerCapture(e.pointerId); } catch (err) {}
    down = true;
    started = performance.now();
    btn.classList.add("active");
    liveSetKey(mod, key);
  });
  const up = () => {
    if (!down) return;
    down = false;
    const wait = Math.max(0, 80 - (performance.now() - started));
    setTimeout(() => {
      btn.classList.remove("active");
      liveSetKey(liveMods, 0);
    }, wait);
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
    down = true;
    btn.classList.add("active");
    livePad.buttons |= bit;
    sendPad();
  });
  const up = () => {
    if (!down) return;
    down = false;
    btn.classList.remove("active");
    livePad.buttons &= ~bit;
    sendPad();
  };
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
    down = true;
    btn.classList.add("active");
    livePad.hat = hat;
    sendPad();
  });
  const up = () => {
    if (!down) return;
    down = false;
    btn.classList.remove("active");
    livePad.hat = LIVE_HAT_NONE;
    sendPad();
  };
  btn.addEventListener("pointerup", up);
  btn.addEventListener("pointercancel", up);
  btn.addEventListener("lostpointercapture", up);
}

function padButton(label, bit) {
  const b = document.createElement("button");
  b.type = "button";
  b.className = "liveControl key";
  b.textContent = label;
  bindPadButton(b, bit);
  return b;
}

function initLive() {
  const rows = [
    [{l:"`",k:53},{l:"1",k:30},{l:"2",k:31},{l:"3",k:32},{l:"4",k:33},{l:"5",k:34},{l:"6",k:35},{l:"7",k:36},{l:"8",k:37},{l:"9",k:38},{l:"0",k:39},{l:"-",k:45},{l:"=",k:46},{l:"Back",k:42,wide:true}],
    [{l:"Tab",k:43,wide:true},{l:"q",k:20},{l:"w",k:26},{l:"e",k:8},{l:"r",k:21},{l:"t",k:23},{l:"y",k:28},{l:"u",k:24},{l:"i",k:12},{l:"o",k:18},{l:"p",k:19},{l:"[",k:47},{l:"]",k:48},{l:"\\",k:49}],
    [{l:"Esc",k:41,wide:true},{l:"a",k:4},{l:"s",k:22},{l:"d",k:7},{l:"f",k:9},{l:"g",k:10},{l:"h",k:11},{l:"j",k:13},{l:"k",k:14},{l:"l",k:15},{l:";",k:51},{l:"'",k:52},{l:"Enter",k:40,wide:true}],
    [{l:"z",k:29},{l:"x",k:27},{l:"c",k:6},{l:"v",k:25},{l:"b",k:5},{l:"n",k:17},{l:"m",k:16},{l:",",k:54},{l:".",k:55},{l:"/",k:56},{l:"Up",k:82},{l:"Left",k:80},{l:"Down",k:81},{l:"Right",k:79}],
    [{l:"Space",k:44,wide:true}]
  ];
  const kb = $("liveKeyboard");
  for (const row of rows) {
    const r = document.createElement("div");
    r.className = "krow";
    for (const item of row) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "liveControl key" + (item.wide ? " wide" : "");
      b.textContent = item.l;
      b.dataset.key = item.k;
      bindKeyButton(b, item.k);
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
    const parts = b.dataset.shortcut.split(",").map(Number);
    bindShortcut(b, parts[0], parts[1]);
  }

  const dpad = [
    { label: "\u25B2", hat: 0, cls: "up" },
    { label: "\u25C0", hat: 2, cls: "left" },
    { label: "\u25B6", hat: 6, cls: "right" },
    { label: "\u25BC", hat: 4, cls: "down" }
  ];
  for (const d of dpad) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "liveControl key " + d.cls;
    b.textContent = d.label;
    bindHatButton(b, d.hat);
    $("liveDpad").appendChild(b);
  }

  const face = [["X", 8], ["Y", 1], ["A", 4], ["B", 2]];
  for (const [label, bit] of face) $("liveFace").appendChild(padButton(label, bit));

  const shoulders = [["L", 16], ["ZL", 64], ["R", 32], ["ZR", 128]];
  for (const [label, bit] of shoulders) $("liveShoulders").appendChild(padButton(label, bit));

  const system = [["-", 256], ["+", 512], ["L3", 1024], ["R3", 2048], ["Home", 4096], ["Capture", 8192]];
  for (const [label, bit] of system) $("liveSystem").appendChild(padButton(label, bit));

  $("liveEnable").addEventListener("change", () => enableLive($("liveEnable").checked));
  $("liveRelease").addEventListener("click", releaseAllLive);
  window.addEventListener("beforeunload", () => {
    if (liveOn && !liveRunning) {
      try { navigator.sendBeacon("/api/live?release=1"); } catch (err) {}
    }
  });
  setLiveControls();
}

async function loadLive() {
  try {
    const d = await jsonFetch("/api/live");
    liveOn = !!d.enabled;
  } catch (e) {
    liveOn = false;
  }
  const en = $("liveEnable");
  if (en) en.checked = liveOn;
  if (liveOn) liveFetch("/api/live?release=1");
  setLiveControls();
}

function wire() {
  $("browse").addEventListener("click", () => $("file").click());
  $("file").addEventListener("change", () => upload($("file").files[0]));
  $("refreshBtn").addEventListener("click", refreshFiles);
  $("startBtn").addEventListener("click", start);
  $("cancelBtn").addEventListener("click", cancel);
  $("savePort").addEventListener("click", savePort);
  $("ktestBtn").addEventListener("click", keyboardTest);
  $("nroBrowse").addEventListener("click", () => $("nroFile").click());
  $("nroUpload").addEventListener("click", uploadUpdate);
  $("nroFile").addEventListener("change", () => {
    const f = $("nroFile").files[0];
    if (f) setMsg($("nroMsg"), f.name + " (" + fmtSize(f.size) + ")", "");
  });
  $("port").addEventListener("keydown", (e) => { if (e.key === "Enter") savePort(); });

  const drop = $("drop");
  for (const ev of ["dragenter", "dragover"]) {
    drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
  }
  for (const ev of ["dragleave", "drop"]) {
    drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
  }
  drop.addEventListener("drop", (e) => {
    const f = e.dataTransfer.files && e.dataTransfer.files[0];
    upload(f);
  });
}

async function boot() {
  wire();
  initLive();
  await loadLive();
  await poll();
  await refreshFiles();
  const s = lastState;
  if (s && s.selected) {
    selected = s.selected;
    renderFiles();
    if (files.some((f) => f.name === selected)) selectFile(selected);
  }
  pollTimer = setInterval(poll, 1000);
  setLiveDebug(true);
}

boot();
