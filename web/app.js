const $ = (id) => document.getElementById(id);

let files = [];
let selected = null;
let lastState = null;
let pollTimer = null;
let debugTimer = null;
let finishedBadge = null;
let finishedTimer = null;

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
        (f.mismatch ? " (file has " + f.lines + ")" : "");
    } else {
      meta.textContent = "header not recognised - " + f.lines + " lines";
    }
    left.appendChild(name);
    left.appendChild(meta);
    const size = document.createElement("div");
    size.className = "size";
    size.textContent = fmtSize(f.size);
    row.appendChild(left);
    row.appendChild(size);
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
