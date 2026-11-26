// The one USB serial link to the badge, owned by this server so every browser
// (local tabs and remote ones through the tunnel) shares it over a WebSocket.
// Clients send raw frames without the sync byte; the server validates, gates on
// badge liveness, coalesces mouse frames under backpressure and adds the sync.
import type http from "node:http";
import { SerialPort, ReadlineParser } from "serialport";
import { WebSocketServer, WebSocket, type RawData } from "ws";

const SYNC = 0xa5;
const OP_MOUSE = 0x01;
const OP_RELEASE = 0x04;
const OP_HEARTBEAT = 0x05;
// Payload length (opcode included) per opcode; must match ../main/super_mouse_usb.c frame_len.
const FRAME_LEN: Record<number, number> = { 0x01: 7, 0x02: 8, 0x03: 6, 0x04: 1, 0x05: 1 };
const ESPRESSIF_VID = "303a";
const BADGE_LIVE_MS = 3000;
const MAX_INFLIGHT = 2;

interface BadgeStatus { ble?: boolean; peer?: string; batt?: number; rx?: number }

let port: SerialPort | null = null;
let portPath = "";
let opening = false;
let autoConnect = process.env.SM_AUTOCONNECT !== "0";
let lastError = "";
let lastStatusMs = 0;
let status: BadgeStatus = {};
let inflight = 0;
let pendingMouse: Buffer | null = null;

const clients = new Set<WebSocket>();
const dirty = new WeakSet<WebSocket>(); // clients that sent input since their last release

function live(): boolean {
  return port !== null && Date.now() - lastStatusMs < BADGE_LIVE_MS;
}

function snapshot() {
  return {
    type: "state",
    usb: port !== null,
    path: portPath,
    live: live(),
    ble: !!status.ble,
    peer: status.peer || "",
    batt: typeof status.batt === "number" ? status.batt : -1,
    clients: clients.size,
    auto: autoConnect,
    error: lastError,
  };
}

let lastSent = "";
function broadcast(force = false): void {
  const msg = JSON.stringify(snapshot());
  if (!force && msg === lastSent) return;
  lastSent = msg;
  for (const ws of clients) if (ws.readyState === WebSocket.OPEN) ws.send(msg);
}

function rawWrite(payload: Buffer): void {
  if (!port) return;
  const out = Buffer.allocUnsafe(payload.length + 1);
  out[0] = SYNC;
  payload.copy(out, 1);
  inflight += 1;
  port.write(out, () => {
    inflight -= 1;
    if (pendingMouse && inflight < MAX_INFLIGHT) {
      const m = pendingMouse;
      pendingMouse = null;
      rawWrite(m);
    }
  });
}

// Merge a mouse frame into the queued one while USB drains; a button change
// flushes the queued frame first so clicks are never merged away.
function writeMouse(f: Buffer): void {
  if (inflight < MAX_INFLIGHT && !pendingMouse) { rawWrite(f); return; }
  if (!pendingMouse) { pendingMouse = Buffer.from(f); return; }
  if (pendingMouse[1] !== f[1]) {
    rawWrite(pendingMouse);
    pendingMouse = Buffer.from(f);
    return;
  }
  const clamp = (v: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, v));
  pendingMouse.writeInt16LE(clamp(pendingMouse.readInt16LE(2) + f.readInt16LE(2), -2048, 2048), 2);
  pendingMouse.writeInt16LE(clamp(pendingMouse.readInt16LE(4) + f.readInt16LE(4), -2048, 2048), 4);
  pendingMouse.writeInt8(clamp(pendingMouse.readInt8(6) + f.readInt8(6), -127, 127), 6);
}

// Input is only forwarded while the badge reports status: frames written while
// the badge app is closed would sit in the USB pipe and replay later.
function handleFrame(ws: WebSocket, data: Buffer): void {
  const op = data[0];
  if (FRAME_LEN[op] !== data.length || op === OP_HEARTBEAT) return;
  if (!live()) return;
  if (op === OP_RELEASE) {
    dirty.delete(ws);
    pendingMouse = null;
    rawWrite(data);
    return;
  }
  dirty.add(ws);
  if (op === OP_MOUSE) writeMouse(data);
  else rawWrite(data);
}

function handleStatusLine(line: string): void {
  const at = line.indexOf("@SM ");
  if (at === -1) return;
  try {
    status = JSON.parse(line.slice(at + 4));
  } catch {
    return;
  }
  lastStatusMs = Date.now();
  broadcast();
}

async function openBadge(): Promise<void> {
  if (port || opening) return;
  opening = true;
  try {
    const list = await SerialPort.list();
    const info = list.find((p) => (p.vendorId || "").toLowerCase() === ESPRESSIF_VID);
    if (!info) { lastError = "Badge not found on the host computer's USB. Plug it in; the server picks it up by itself."; return; }
    // macOS lists both /dev/tty.* and /dev/cu.*; cu.* does not wait for carrier detect.
    const p = info.path.replace(/^\/dev\/tty\./, "/dev/cu.");
    const sp = new SerialPort({ path: p, baudRate: 115200, autoOpen: false });
    await new Promise<void>((resolve, reject) => sp.open((err) => (err ? reject(err) : resolve())));
    port = sp;
    portPath = p;
    inflight = 0;
    pendingMouse = null;
    lastStatusMs = 0;
    lastError = "";
    console.log(`[badge] opened ${p}`);
    const lines = sp.pipe(new ReadlineParser({ delimiter: "\n" }));
    lines.on("data", (l: string) => handleStatusLine(l));
    sp.on("close", () => dropPort("closed"));
    sp.on("error", (e) => dropPort(e.message));
  } catch (err) {
    lastError = `Could not open the badge: ${(err as Error).message}. Close idf.py monitor or other serial apps.`;
  } finally {
    opening = false;
    broadcast();
  }
}

function dropPort(reason: string): void {
  if (!port) return;
  const p = port;
  port = null;
  portPath = "";
  lastStatusMs = 0;
  status = {};
  pendingMouse = null;
  if (reason !== "closed") lastError = reason;
  console.log(`[badge] link down: ${reason}`);
  if (p.isOpen) p.close(() => {});
  broadcast();
}

function closeBadge(): void {
  if (!port) return;
  if (live()) rawWrite(Buffer.from([OP_RELEASE]));
  const p = port;
  // Give the release frame a moment to reach the badge.
  setTimeout(() => { if (port === p) dropPort("closed"); }, 150);
}

function handleControl(text: string): void {
  let msg: { cmd?: unknown };
  try { msg = JSON.parse(text); } catch { return; }
  if (msg.cmd === "connect") { autoConnect = true; lastError = ""; void openBadge(); }
  else if (msg.cmd === "disconnect") { autoConnect = false; lastError = ""; closeBadge(); }
  broadcast(true);
}

/** Attaches the shared link to `server` at /ws. `allow` decides who may upgrade. */
export function attachBadgeLink(server: http.Server, allow: (req: http.IncomingMessage) => boolean): void {
  const wss = new WebSocketServer({ noServer: true, maxPayload: 4096 });

  server.on("upgrade", (req, socket, head) => {
    const p = new URL(req.url || "/", "http://localhost").pathname;
    if (p !== "/ws" || !allow(req)) {
      socket.write("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n");
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws, req));
  });

  wss.on("connection", (ws) => {
    clients.add(ws);
    let alive = true;
    ws.on("pong", () => { alive = true; });
    const ping = setInterval(() => {
      if (!alive) { ws.terminate(); return; }
      alive = false;
      ws.ping();
    }, 20000);
    ws.on("message", (data: RawData, isBinary: boolean) => {
      const buf = Array.isArray(data) ? Buffer.concat(data) : Buffer.from(data as Buffer);
      if (isBinary) handleFrame(ws, buf);
      else handleControl(buf.toString("utf8"));
    });
    ws.on("close", () => {
      clearInterval(ping);
      clients.delete(ws);
      // A tab that vanished mid-chord must not leave keys or buttons held.
      if (dirty.has(ws) && live()) rawWrite(Buffer.from([OP_RELEASE]));
      broadcast();
    });
    ws.send(JSON.stringify(snapshot()));
    broadcast();
  });

  // Heartbeat keeps the badge's "web app live" state while anyone is watching;
  // with no clients the badge times out and releases everything on its own.
  setInterval(() => {
    if (port && clients.size > 0 && inflight === 0) rawWrite(Buffer.from([OP_HEARTBEAT]));
    if (!port && autoConnect) void openBadge();
    broadcast();
  }, 1000);
}
