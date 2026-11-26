import {
  CODE_TO_HID,
  CODE_TO_MOD,
  MOD_LMETA,
  asciiToHid,
  formatModBits,
} from "/hid-map.js";

const OP_MOUSE = 0x01;
const OP_KBD = 0x02;
const OP_MACRO = 0x03;
const OP_RELEASE = 0x04;
// The server owns the USB serial port; this page shares it over /ws with every
// other signed-in tab. Frames go out without the 0xA5 sync byte (server adds it).
const WS_BACKLOG = 64; // bytes queued on the socket before mouse frames coalesce

const $ = (id) => document.getElementById(id);

const linkDot = $("linkDot");
const badgePill = $("badgePill");
const blePill = $("blePill");
const battPill = $("battPill");
const usbPill = $("usbPill");
const connectBtn = $("connectBtn");
const releaseBtn = $("releaseBtn");
const trackpad = $("trackpad");
const activeKeys = $("activeKeys");
const sensSlider = $("sensSlider");
const sensVal = $("sensVal");
const invertScroll = $("invertScroll");
const capsCmd = $("capsCmd");
const typeInput = $("typeInput");
const typeSendBtn = $("typeSendBtn");
const typeStopBtn = $("typeStopBtn");
const typeCopyBtn = $("typeCopyBtn");
const typeClearBtn = $("typeClearBtn");
const typeStats = $("typeStats");
const themeToggleBtn = $("themeToggleBtn");
const serialHint = $("serialHint");

const THEME_STORAGE_KEY = "super-mouse-theme";
let typeSending = false;
let typeSendAbort = false;

// Shared link: WebSocket to the server, which holds the badge's USB port
let ws = null;
let wsRetryMs = 500;
let link = { usb: false, live: false, ble: false, peer: "", batt: -1, clients: 0, auto: true, error: "", path: "" };
let sentInput = false; // this tab sent input since its last release

// Mouse state (leading-edge throttled, paced to the BLE connection interval)
let mouseButtons = 0;
let accDx = 0;
let accDy = 0;
let accWheel = 0;
let mouseFlushTimer = null;
let lastMouseSendMs = 0;
const mouseIntervalMs = 15;
let draggingFallback = false;
let capturedMouseDown = false;
let lastFallbackX = 0;
let lastFallbackY = 0;

// Pre-allocated binary frame buffers to avoid per-event GC churn
const mouseBuf = new ArrayBuffer(7);
const mouseView = new DataView(mouseBuf);
const kbdFrame = new Uint8Array(8);
const macroFrame = new Uint8Array(6);
const releaseFrame = new Uint8Array([OP_RELEASE]);

// 6KRO keyboard state
let physMods = 0;
const heldKeys = []; // up to 6 USB HID usage IDs

// Sticky modifiers: 0 = off, 1 = armed (one-shot), 2 = locked
const stickyState = new Map([
  [1, 0], // Ctrl
  [2, 0], // Shift
  [4, 0], // Alt
  [8, 0], // Cmd/Win
]);

// App switcher held modifier (8 = Cmd, 4 = Alt, 0 = inactive)
// Stays held after tapping Tab until a mouse click (or Release All) is registered.
let switcherMod = 0;

function effectiveMods() {
  let m = physMods | switcherMod;
  for (const [bit, st] of stickyState.entries()) {
    if (st > 0) m |= bit;
  }
  return m;
}

function wsOpen() {
  return ws !== null && ws.readyState === WebSocket.OPEN;
}

function linkOpen() {
  return wsOpen() && link.usb;
}

function badgeLive() {
  return linkOpen() && link.live;
}

function wsBacklogged() {
  return ws !== null && ws.bufferedAmount > WS_BACKLOG;
}

// The server drops input while the badge is not reporting status (no stale
// replay); checking here too just saves the round trip.
function sendBin(arr) {
  if (!badgeLive()) return;
  const src = arr instanceof ArrayBuffer ? new Uint8Array(arr) : arr;
  if (src[0] === OP_RELEASE) sentInput = false;
  else sentInput = true;
  ws.send(src.slice());
}

function sendMouse(buttons, dx, dy, wheel) {
  mouseView.setUint8(0, OP_MOUSE);
  mouseView.setUint8(1, buttons & 0x07);
  mouseView.setInt16(2, Math.max(-2048, Math.min(2048, Math.round(dx))), true);
  mouseView.setInt16(4, Math.max(-2048, Math.min(2048, Math.round(dy))), true);
  mouseView.setInt8(6, Math.max(-127, Math.min(127, Math.round(wheel))));
  sendBin(mouseBuf);
  lastMouseSendMs = performance.now();
}

function flushMouse(force = false) {
  if (mouseFlushTimer) {
    clearTimeout(mouseFlushTimer);
    mouseFlushTimer = null;
  }
  const dx = Math.round(accDx);
  const dy = Math.round(accDy);
  const wh = Math.round(accWheel);
  if (!force && dx === 0 && dy === 0 && wh === 0) return;
  if (!force && wsBacklogged()) {
    // Keep coalescing while USB drains instead of piling up stale frames
    mouseFlushTimer = setTimeout(() => flushMouse(false), mouseIntervalMs);
    return;
  }
  accDx = 0;
  accDy = 0;
  accWheel = 0;
  sendMouse(mouseButtons, dx, dy, wh);
}

function scheduleMouseFlush() {
  if (!badgeLive()) return;
  const elapsed = performance.now() - lastMouseSendMs;
  if (elapsed >= mouseIntervalMs && !wsBacklogged()) {
    flushMouse(false);
  } else if (!mouseFlushTimer) {
    const wait = Math.max(1, Math.ceil(mouseIntervalMs - elapsed));
    mouseFlushTimer = setTimeout(() => flushMouse(false), wait);
  }
}

function sendKbdState() {
  const mod = effectiveMods();
  kbdFrame[0] = OP_KBD;
  kbdFrame[1] = mod;
  for (let i = 0; i < 6; i++) {
    kbdFrame[2 + i] = heldKeys[i] || 0;
  }
  sendBin(kbdFrame);
  renderActiveKeys(mod);
}

function sendMacro(mod, keys, holdMs = 35) {
  macroFrame[0] = OP_MACRO;
  macroFrame[1] = mod & 0xFF;
  macroFrame[2] = keys[0] || 0;
  macroFrame[3] = keys[1] || 0;
  macroFrame[4] = keys[2] || 0;
  macroFrame[5] = holdMs;
  sendBin(macroFrame);
}

function releaseAll() {
  typeSendAbort = true;
  const wasActive = sentInput || switcherMod !== 0;
  physMods = 0;
  switcherMod = 0;
  heldKeys.length = 0;
  mouseButtons = 0;
  accDx = accDy = accWheel = 0;
  if (mouseFlushTimer) {
    clearTimeout(mouseFlushTimer);
    mouseFlushTimer = null;
  }
  for (const bit of stickyState.keys()) stickyState.set(bit, 0);
  renderStickyButtons();
  renderSwitcherButtons();
  renderActiveKeys(0);
  // Only a tab that pressed something releases, so blurring an idle window
  // does not cut off a drag another window is doing.
  if (wasActive) sendBin(releaseFrame);
}

function renderSwitcherButtons() {
  document.querySelectorAll(".switch-btn").forEach((btn) => {
    const bit = Number(btn.dataset.switchMod);
    btn.classList.toggle("locked", switcherMod === bit);
  });
}

function releaseSwitcherIfActive() {
  if (!switcherMod) return;
  switcherMod = 0;
  renderSwitcherButtons();
  sendKbdState();
}

function triggerSwitcher(modBit) {
  const HID_TAB = 0x2B;
  switcherMod = modBit;
  renderSwitcherButtons();
  const mod = effectiveMods();
  // 1) Press Mod + Tab
  const down = new Uint8Array([OP_KBD, mod, HID_TAB, 0, 0, 0, 0, 0]);
  sendBin(down);
  renderActiveKeys(mod);
  // 2) Release Tab after 35 ms, keeping Mod held until a click is registered
  setTimeout(() => {
    if (switcherMod === modBit) sendKbdState();
  }, 35);
}

function renderActiveKeys(mod) {
  const parts = formatModBits(mod);
  for (const k of heldKeys) parts.push(`0x${k.toString(16).toUpperCase().padStart(2, "0")}`);
  activeKeys.textContent = parts.length ? `Active keys: ${parts.join(" + ")}` : "Active keys: none";
}

function renderStickyButtons() {
  document.querySelectorAll(".mod-btn").forEach((btn) => {
    const bit = Number(btn.dataset.mod);
    const st = stickyState.get(bit) || 0;
    btn.classList.toggle("armed", st === 1);
    btn.classList.toggle("locked", st === 2);
  });
}

function consumeOneShotSticky() {
  let changed = false;
  for (const [bit, st] of stickyState.entries()) {
    if (st === 1) {
      stickyState.set(bit, 0);
      changed = true;
    }
  }
  if (changed) renderStickyButtons();
}

function renderLink() {
  const up = wsOpen();
  const open = linkOpen();
  const live = badgeLive();
  linkDot.classList.toggle("ok", live);
  connectBtn.disabled = !up;
  connectBtn.textContent = open ? "Disconnect Badge" : "Connect Badge (USB)";
  const others = Math.max(0, link.clients - 1);
  const share = others ? ` · ${others} other tab${others > 1 ? "s" : ""}` : "";
  if (!up) {
    usbPill.textContent = "Server: offline";
    usbPill.className = "pill warn";
  } else {
    usbPill.textContent = open ? `USB: connected${share}` : `USB: --${share}`;
    usbPill.className = open ? "pill ok" : "pill";
  }
  if (!open) {
    badgePill.textContent = "Badge: --";
    badgePill.className = "pill";
    blePill.textContent = "BLE: --";
    blePill.className = "pill";
  } else if (!live) {
    badgePill.textContent = "Badge: open Super Mouse";
    badgePill.className = "pill warn";
    blePill.textContent = "BLE: --";
    blePill.className = "pill";
  } else {
    badgePill.textContent = "Badge: Super Mouse";
    badgePill.className = "pill ok";
    blePill.textContent = link.ble ? `BLE: ${link.peer || "connected"}` : "BLE: advertising";
    blePill.className = link.ble ? "pill ok" : "pill warn";
  }
  if (link.batt >= 0) battPill.textContent = `Batt: ${link.batt}%`;

  if (!up) serialHint.textContent = "Reconnecting to the Super Mouse server...";
  else if (link.error && !open) serialHint.textContent = link.error;
  else if (!open) serialHint.textContent = link.auto
    ? "Waiting for the badge on the host computer's USB..."
    : "Badge link is off. Click Connect Badge (USB).";
  else if (!live) serialHint.textContent = "Connected. Open Super Mouse on the badge.";
  else serialHint.textContent = `Connected on the host computer (${link.path}). Shared with every signed-in tab.`;
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  const sock = new WebSocket(`${proto}//${location.host}/ws`);
  sock.binaryType = "arraybuffer";
  ws = sock;
  sock.addEventListener("open", () => {
    wsRetryMs = 500;
    renderLink();
  });
  sock.addEventListener("message", (e) => {
    if (typeof e.data !== "string") return;
    let msg;
    try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type !== "state") return;
    const wasLive = badgeLive();
    link = msg;
    if (wasLive && !badgeLive()) releaseLocalState();
    renderLink();
  });
  sock.addEventListener("close", () => {
    if (ws !== sock) return;
    ws = null;
    link = { ...link, usb: false, live: false };
    releaseLocalState();
    renderLink();
    // 401 on upgrade surfaces as an abnormal close; the page fetch tells us if the session expired.
    fetch("/", { method: "HEAD", redirect: "manual" }).then((r) => {
      if (r.type === "opaqueredirect" || r.status === 302 || r.status === 401) location.href = "/login";
    }).catch(() => {});
    setTimeout(connectWs, wsRetryMs);
    wsRetryMs = Math.min(wsRetryMs * 2, 8000);
  });
}

// Drop held state locally when the link goes away (the badge/server release on their own).
function releaseLocalState() {
  sentInput = false;
  physMods = 0;
  switcherMod = 0;
  heldKeys.length = 0;
  mouseButtons = 0;
  accDx = accDy = accWheel = 0;
  renderSwitcherButtons();
  renderActiveKeys(effectiveMods());
}

// ── Pointer Lock & Mouse Handling ───────────────────────────────────────────
function isCaptured() {
  return document.pointerLockElement === trackpad;
}

trackpad.addEventListener("click", () => {
  trackpad.focus();
  if (!isCaptured() && trackpad.requestPointerLock) {
    trackpad.requestPointerLock();
  }
});

document.addEventListener("pointerlockchange", () => {
  const cap = isCaptured();
  trackpad.classList.toggle("captured", cap);
  if (!cap) releaseAll();
});

function domButtonToBit(btn) {
  if (btn === 0) return 1; // Left
  if (btn === 2) return 2; // Right
  if (btn === 1) return 4; // Middle
  return 0;
}

trackpad.addEventListener("contextmenu", (e) => e.preventDefault());

trackpad.addEventListener("mousedown", (e) => {
  if (!isCaptured()) {
    draggingFallback = true;
    lastFallbackX = e.clientX;
    lastFallbackY = e.clientY;
    return;
  }
  e.preventDefault();
  capturedMouseDown = true;
  mouseButtons |= domButtonToBit(e.button);
  flushMouse(true);
});

window.addEventListener("mouseup", (e) => {
  if (draggingFallback) {
    draggingFallback = false;
    return;
  }
  if (!isCaptured()) {
    capturedMouseDown = false;
    return;
  }
  mouseButtons &= ~domButtonToBit(e.button);
  flushMouse(true);
  if (capturedMouseDown) {
    capturedMouseDown = false;
    releaseSwitcherIfActive();
  }
});

window.addEventListener("mousemove", (e) => {
  const sens = Number(sensSlider.value);
  if (isCaptured()) {
    accDx += e.movementX * sens;
    accDy += e.movementY * sens;
    scheduleMouseFlush();
  } else if (draggingFallback) {
    accDx += (e.clientX - lastFallbackX) * sens;
    accDy += (e.clientY - lastFallbackY) * sens;
    lastFallbackX = e.clientX;
    lastFallbackY = e.clientY;
    scheduleMouseFlush();
  }
});

trackpad.addEventListener("wheel", (e) => {
  if (!isCaptured() && document.activeElement !== trackpad) return;
  e.preventDefault();
  const dir = e.deltaY > 0 ? 1 : -1;
  accWheel += invertScroll.checked ? -dir : dir;
  scheduleMouseFlush();
}, { passive: false });

// ── 6KRO Keyboard Capture ───────────────────────────────────────────────────
function shouldCaptureKey(e) {
  if (isCaptured()) return true;
  return document.activeElement === trackpad;
}

window.addEventListener("keydown", (e) => {
  if (!shouldCaptureKey(e)) return;
  // Let Esc unlock Pointer Lock naturally if user isn't holding modifiers
  if (e.code === "Escape" && isCaptured() && effectiveMods() === 0) return;

  e.preventDefault();
  if (e.repeat) return;

  // Optional CapsLock -> Cmd remap so physical CapsLock + Space sends Cmd + Space
  if (e.code === "CapsLock" && capsCmd.checked) {
    physMods |= MOD_LMETA;
    sendKbdState();
    return;
  }

  if (CODE_TO_MOD[e.code]) {
    physMods |= CODE_TO_MOD[e.code];
    sendKbdState();
    return;
  }

  const hid = CODE_TO_HID[e.code];
  if (hid && !heldKeys.includes(hid) && heldKeys.length < 6) {
    heldKeys.push(hid);
    sendKbdState();
  }
});

window.addEventListener("keyup", (e) => {
  if (!shouldCaptureKey(e)) return;
  e.preventDefault();

  if (e.code === "CapsLock" && capsCmd.checked) {
    physMods &= ~MOD_LMETA;
    sendKbdState();
    return;
  }

  if (CODE_TO_MOD[e.code]) {
    physMods &= ~CODE_TO_MOD[e.code];
    sendKbdState();
    return;
  }

  const hid = CODE_TO_HID[e.code];
  if (hid) {
    const idx = heldKeys.indexOf(hid);
    if (idx !== -1) heldKeys.splice(idx, 1);
    consumeOneShotSticky();
    sendKbdState();
  }
});

window.addEventListener("blur", releaseAll);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) releaseAll();
});

// ── UI Buttons, Sticky Modifiers, Macros & Text Typing ──────────────────────
sensSlider.addEventListener("input", () => {
  sensVal.textContent = `${Number(sensSlider.value).toFixed(1)}x`;
});

$("btnLeft").addEventListener("click", () => {
  sendMouse(1, 0, 0, 0);
  setTimeout(() => { sendMouse(0, 0, 0, 0); releaseSwitcherIfActive(); }, 30);
});
$("btnMiddle").addEventListener("click", () => {
  sendMouse(4, 0, 0, 0);
  setTimeout(() => { sendMouse(0, 0, 0, 0); releaseSwitcherIfActive(); }, 30);
});
$("btnRight").addEventListener("click", () => {
  sendMouse(2, 0, 0, 0);
  setTimeout(() => { sendMouse(0, 0, 0, 0); releaseSwitcherIfActive(); }, 30);
});
$("btnScrollUp").addEventListener("click", () => sendMouse(0, 0, 0, 3));
$("btnScrollDown").addEventListener("click", () => sendMouse(0, 0, 0, -3));

document.querySelectorAll(".switch-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    triggerSwitcher(Number(btn.dataset.switchMod));
    trackpad.focus();
  });
});

document.querySelectorAll(".mod-btn").forEach((btn) => {
  const bit = Number(btn.dataset.mod);
  btn.addEventListener("click", () => {
    const cur = stickyState.get(bit) || 0;
    stickyState.set(bit, cur === 0 ? 1 : 0);
    renderStickyButtons();
    renderActiveKeys(effectiveMods());
    trackpad.focus();
  });
  btn.addEventListener("dblclick", () => {
    stickyState.set(bit, 2);
    renderStickyButtons();
    renderActiveKeys(effectiveMods());
    trackpad.focus();
  });
});

document.querySelectorAll(".macro-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    const mod = Number(btn.dataset.mod || 0);
    const keys = (btn.dataset.keys || "").split(",").filter(Boolean).map(Number);
    sendMacro(mod, keys, 35);
  });
});

function applyTheme(theme) {
  const isLight = theme === "light";
  if (isLight) {
    document.documentElement.dataset.theme = "light";
  } else {
    delete document.documentElement.dataset.theme;
  }
  themeToggleBtn.textContent = isLight ? "Theme: Light" : "Theme: Dark";
  try {
    localStorage.setItem(THEME_STORAGE_KEY, isLight ? "light" : "dark");
  } catch {
    // ignore storage errors
  }
}

function initTheme() {
  let saved = "dark";
  try {
    saved = localStorage.getItem(THEME_STORAGE_KEY) || "dark";
  } catch {
    // ignore storage errors
  }
  applyTheme(saved);
}

themeToggleBtn.addEventListener("click", () => {
  const next = document.documentElement.dataset.theme === "light" ? "dark" : "light";
  applyTheme(next);
});

function updateTypeStats(progressText = null) {
  if (progressText) {
    typeStats.textContent = progressText;
    return;
  }
  const val = typeInput.value;
  if (!val) {
    typeStats.textContent = "0 chars";
    return;
  }
  const lines = val.split(/\r\n|\r|\n/).length;
  typeStats.textContent = lines > 1
    ? `${val.length} chars · ${lines} lines`
    : `${val.length} chars`;
}

async function sendTextString(str) {
  if (typeSending) return;
  const normalized = str.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  typeSending = true;
  typeSendAbort = false;
  typeSendBtn.disabled = true;
  typeStopBtn.hidden = false;
  try {
    let sent = 0;
    const total = normalized.length;
    for (const ch of normalized) {
      if (typeSendAbort) break;
      sent += 1;
      const m = asciiToHid(ch);
      if (!m) continue;
      sendMacro(m.mod, [m.hid], 20);
      updateTypeStats(`Sending ${sent}/${total}...`);
      await new Promise((r) => setTimeout(r, 38));
    }
  } finally {
    typeSending = false;
    typeSendAbort = false;
    typeSendBtn.disabled = false;
    typeStopBtn.hidden = true;
    updateTypeStats();
  }
}

typeInput.addEventListener("input", () => updateTypeStats());

typeSendBtn.addEventListener("click", () => {
  const txt = typeInput.value;
  if (!txt) return;
  sendTextString(txt);
});

typeStopBtn.addEventListener("click", () => {
  typeSendAbort = true;
});

typeCopyBtn.addEventListener("click", async () => {
  if (!typeInput.value) return;
  await navigator.clipboard.writeText(typeInput.value);
  typeCopyBtn.textContent = "Copied";
  setTimeout(() => { typeCopyBtn.textContent = "Copy"; }, 1200);
});

typeClearBtn.addEventListener("click", () => {
  typeInput.value = "";
  updateTypeStats();
  typeInput.focus();
});

typeInput.addEventListener("keydown", (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
    e.preventDefault();
    typeSendBtn.click();
    return;
  }
  if (e.key === "Tab") {
    e.preventDefault();
    const start = typeInput.selectionStart;
    const end = typeInput.selectionEnd;
    typeInput.setRangeText("\t", start, end, "end");
    updateTypeStats();
  }
});

initTheme();
updateTypeStats();

connectBtn.addEventListener("click", () => {
  if (!wsOpen()) return;
  if (linkOpen()) releaseAll();
  ws.send(JSON.stringify({ cmd: linkOpen() ? "disconnect" : "connect" }));
});
releaseBtn.addEventListener("click", releaseAll);

renderLink();
connectWs();
