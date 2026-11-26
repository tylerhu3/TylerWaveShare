// Login page: collects a browser fingerprint to send with each attempt and
// renders the attempt log. Log values are attacker-controlled, so only textContent.
const $ = (id) => document.getElementById(id);

async function sha256(text) {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function canvasData() {
  try {
    const c = document.createElement("canvas");
    c.width = 240; c.height = 60;
    const g = c.getContext("2d");
    g.textBaseline = "top";
    g.font = "16px 'Arial'";
    g.fillStyle = "#f60"; g.fillRect(100, 1, 62, 20);
    g.fillStyle = "#069"; g.fillText("Super Mouse \u{1F5B1} 1.75C", 2, 15);
    g.fillStyle = "rgba(102, 204, 0, 0.7)"; g.fillText("Super Mouse \u{1F5B1} 1.75C", 4, 17);
    return c.toDataURL();
  } catch { return ""; }
}

function webgl() {
  try {
    const gl = document.createElement("canvas").getContext("webgl");
    if (!gl) return null;
    const dbg = gl.getExtension("WEBGL_debug_renderer_info");
    return {
      vendor: dbg ? gl.getParameter(dbg.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR),
      renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
      version: gl.getParameter(gl.VERSION),
    };
  } catch { return null; }
}

async function collectClient() {
  const n = navigator;
  const info = {
    timezone: Intl.DateTimeFormat().resolvedOptions().timeZone,
    tzOffsetMin: new Date().getTimezoneOffset(),
    locale: Intl.DateTimeFormat().resolvedOptions().locale,
    languages: n.languages ? [...n.languages] : [n.language],
    platform: n.platform,
    vendor: n.vendor,
    hardwareConcurrency: n.hardwareConcurrency ?? null,
    deviceMemoryGB: n.deviceMemory ?? null,
    maxTouchPoints: n.maxTouchPoints ?? 0,
    cookieEnabled: n.cookieEnabled,
    doNotTrack: n.doNotTrack ?? null,
    webdriver: n.webdriver ?? null,
    pdfViewer: n.pdfViewerEnabled ?? null,
    screen: {
      width: screen.width, height: screen.height,
      availWidth: screen.availWidth, availHeight: screen.availHeight,
      colorDepth: screen.colorDepth, pixelRatio: devicePixelRatio,
      orientation: screen.orientation?.type ?? null,
    },
    viewport: { width: innerWidth, height: innerHeight },
    prefersDark: matchMedia("(prefers-color-scheme: dark)").matches,
    reducedMotion: matchMedia("(prefers-reduced-motion: reduce)").matches,
    connection: n.connection ? { type: n.connection.effectiveType, rttMs: n.connection.rtt, downlinkMbps: n.connection.downlink, saveData: n.connection.saveData } : null,
    webgl: webgl(),
    webSerial: "serial" in n,
    referrer: document.referrer || null,
    historyLength: history.length,
  };
  if (n.userAgentData?.getHighEntropyValues) {
    try {
      info.uaData = await n.userAgentData.getHighEntropyValues(["platform", "platformVersion", "architecture", "bitness", "model", "fullVersionList", "mobile"]);
    } catch {}
  }
  if (n.getBattery) {
    try { const b = await n.getBattery(); info.battery = { level: b.level, charging: b.charging }; } catch {}
  }
  info.canvasHash = (await sha256(canvasData())).slice(0, 16);
  // Stable across visits from the same browser/device; excludes volatile fields.
  const stable = { ...info, viewport: undefined, battery: undefined, connection: undefined, referrer: undefined, historyLength: undefined };
  info.fingerprint = (await sha256(JSON.stringify(stable))).slice(0, 16);
  return info;
}

const clientInfo = collectClient();

// ---------- status / lockout countdown ----------

let lockTimer = 0;

function fmtDuration(sec) {
  if (sec < 60) return `${sec}s`;
  if (sec < 3600) return `${Math.floor(sec / 60)}m ${sec % 60}s`;
  return `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m`;
}

function showLock(lockedUntil, scope) {
  clearInterval(lockTimer);
  const until = Date.parse(lockedUntil);
  const tick = () => {
    const left = Math.ceil((until - Date.now()) / 1000);
    if (left <= 0) {
      clearInterval(lockTimer);
      $("loginBtn").disabled = false;
      setMsg("You can try again now.", "");
      return;
    }
    $("loginBtn").disabled = true;
    setMsg(`Too many wrong passwords${scope === "global" ? " from everyone" : " from your address"}. Try again in ${fmtDuration(left)}.`, "err");
  };
  tick();
  lockTimer = setInterval(tick, 1000);
}

function setMsg(text, cls) {
  const m = $("loginMsg");
  m.textContent = text;
  m.className = `auth-msg ${cls}`;
}

function geoText(geo = {}) {
  return [geo.city, geo.region, geo.country].filter(Boolean).join(", ");
}

async function refreshStatus() {
  const s = await (await fetch("/auth/status", { cache: "no-store" })).json();
  $("signedInControls").hidden = !s.authed;
  $("loginCard").hidden = s.authed;
  $("publicNote").hidden = !s.publicLog;
  $("youLine").textContent = `You appear as ${s.you.ip}${geoText(s.you.geo) ? ` (${geoText(s.you.geo)})` : ""}.`;
  if (s.locked) showLock(s.lockedUntil, s.scope);
  $("logCard").hidden = !s.publicLog && !s.authed;
  if (s.publicLog || s.authed) loadLog();
  return s;
}

// ---------- login ----------

$("loginForm").addEventListener("submit", async (e) => {
  e.preventDefault();
  $("loginBtn").disabled = true;
  setMsg("Checking…", "");
  try {
    const r = await fetch("/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: $("password").value, username: e.target.username.value, client: await clientInfo }),
    });
    const body = await r.json().catch(() => ({}));
    if (r.ok) {
      setMsg("Signed in.", "ok");
      location.href = "/";
      return;
    }
    $("password").value = "";
    if (body.locked) showLock(body.lockedUntil, body.scope);
    else {
      $("loginBtn").disabled = false;
      setMsg(body.freeAttemptsLeft > 0
        ? `Wrong password. ${body.freeAttemptsLeft} more before a lockout.`
        : `Wrong password. Next mistake locks you out for ${fmtDuration(body.nextLockSec)}.`, "err");
    }
  } catch {
    $("loginBtn").disabled = false;
    setMsg("Network error.", "err");
  }
  loadLog();
});

$("logoutBtn").addEventListener("click", async () => {
  await fetch("/auth/logout", { method: "POST" });
  refreshStatus();
});

// ---------- attempt log ----------

function td(text, cls) {
  const el = document.createElement("td");
  el.textContent = text ?? "";
  if (cls) el.className = cls;
  return el;
}

function shortUa(a) {
  const ch = a.clientHints || {};
  const brand = (ch["sec-ch-ua"] || "").match(/"((?:Google Chrome|Microsoft Edge|Chromium|Brave|Opera)[^"]*)";v="(\d+)/);
  const plat = (ch["sec-ch-ua-platform"] || "").replace(/"/g, "");
  if (brand) return `${brand[1]} ${brand[2]}${plat ? ` · ${plat}` : ""}`;
  const ua = a.userAgent || "";
  const b = ua.match(/(Firefox|Edg|OPR|Chrome|Safari|curl|python-requests|Go-http-client|Wget)\/[\d.]+/);
  const os = ua.match(/\(([^;)]+)/);
  return [b?.[0], os?.[1]].filter(Boolean).join(" · ") || ua.slice(0, 60) || "(no user agent)";
}

async function loadLog() {
  const r = await fetch("/auth/attempts", { cache: "no-store" });
  if (!r.ok) return;
  const { attempts } = await r.json();
  const tbody = $("logBody");
  tbody.replaceChildren();
  const counts = { success: 0, failure: 0, locked: 0 };
  for (const a of attempts) {
    counts[a.result] = (counts[a.result] || 0) + 1;
    const tr = document.createElement("tr");
    tr.className = `res-${a.result}`;
    const when = new Date(a.time);
    const tdTime = td(when.toLocaleString());
    tdTime.title = a.time;
    tr.append(
      tdTime,
      td(a.result + (a.failCount ? ` #${a.failCount}` : ""), "res"),
      td(a.ip + (a.rdns?.length ? `\n${a.rdns[0]}` : ""), "mono"),
      td(geoText(a.geo) || "—"),
      td(shortUa(a)),
      td(a.fingerprint || "—", "mono"),
    );
    const detail = document.createElement("td");
    const d = document.createElement("details");
    const sum = document.createElement("summary");
    sum.textContent = "all";
    const pre = document.createElement("pre");
    pre.textContent = JSON.stringify(a, null, 2);
    d.append(sum, pre);
    detail.append(d);
    tr.append(detail);
    tbody.append(tr);
  }
  $("logCount").textContent = `${attempts.length} shown · ${counts.success} ok · ${counts.failure} wrong · ${counts.locked} blocked`;
}

refreshStatus();
setInterval(loadLog, 15000);
