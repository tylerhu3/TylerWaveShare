import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { fileURLToPath } from "node:url";
import { WebSocketServer, WebSocket, type RawData } from "ws";
import { isValidHidFrame, OP_MOUSE, OP_RELEASE, RELEASE_ALL_FRAME } from "./protocol.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.resolve(__dirname, "../public");

const PORT = Number(process.env.PORT || 8765);
const HOST = process.env.HOST || "0.0.0.0";
const AUTH_TOKEN = process.env.SUPER_MOUSE_TOKEN || "";

interface Room {
  device: WebSocket | null;
  clients: Set<WebSocket>;
  lastStatus: Record<string, unknown> | null;
  inFlight: boolean;
  hasPendingMouse: boolean;
  pendDx: number;
  pendDy: number;
  pendWheel: number;
  pendButtons: number;
  lastButtons: number;
}

const rooms = new Map<string, Room>();

function getRoom(id: string): Room {
  let r = rooms.get(id);
  if (!r) {
    r = {
      device: null,
      clients: new Set(),
      lastStatus: null,
      inFlight: false,
      hasPendingMouse: false,
      pendDx: 0,
      pendDy: 0,
      pendWheel: 0,
      pendButtons: 0,
      lastButtons: 0,
    };
    rooms.set(id, r);
  }
  return r;
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

function resetPendingMouse(room: Room): void {
  room.hasPendingMouse = false;
  room.pendDx = 0;
  room.pendDy = 0;
  room.pendWheel = 0;
}

function flushPendingMouse(room: Room): void {
  const dev = room.device;
  if (!room.hasPendingMouse || !dev || dev.readyState !== WebSocket.OPEN) {
    resetPendingMouse(room);
    return;
  }
  const frame = Buffer.allocUnsafe(7);
  frame[0] = OP_MOUSE;
  frame[1] = room.pendButtons & 0x07;
  frame.writeInt16LE(clamp(room.pendDx, -2048, 2048), 2);
  frame.writeInt16LE(clamp(room.pendDy, -2048, 2048), 4);
  frame.writeInt8(clamp(room.pendWheel, -127, 127), 6);
  room.lastButtons = room.pendButtons & 0x07;
  resetPendingMouse(room);
  sendDeviceBinary(room, frame);
}

function sendDeviceBinary(room: Room, buf: Buffer): void {
  const dev = room.device;
  if (!dev || dev.readyState !== WebSocket.OPEN) return;
  room.inFlight = true;
  dev.send(buf, { binary: true }, () => {
    room.inFlight = false;
    if (room.hasPendingMouse) flushPendingMouse(room);
  });
}

function getLanIps(): string[] {
  const ips: string[] = [];
  const nets = os.networkInterfaces();
  for (const list of Object.values(nets)) {
    if (!list) continue;
    for (const info of list) {
      if (info.family === "IPv4" && !info.internal) ips.push(info.address);
    }
  }
  return ips;
}

function broadcastJson(targets: Iterable<WebSocket>, payload: Record<string, unknown>): void {
  const txt = JSON.stringify(payload);
  for (const ws of targets) {
    if (ws.readyState === WebSocket.OPEN) ws.send(txt);
  }
}

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".webmanifest": "application/manifest+json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".ico": "image/x-icon",
};

const server = http.createServer((req, res) => {
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);

  if (url.pathname === "/api/info") {
    const ips = getLanIps();
    res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" });
    res.end(JSON.stringify({
      port: PORT,
      lanIps: ips,
      authRequired: AUTH_TOKEN.length > 0,
      defaultDeviceUrl: `ws://${ips[0] || "127.0.0.1"}:${PORT}/ws/device?room=badge1`,
    }));
    return;
  }

  let filePath = url.pathname === "/" ? "/index.html" : url.pathname;
  filePath = path.normalize(filePath).replace(/^(\.\.[/\\])+/, "");
  const fullPath = path.join(PUBLIC_DIR, filePath);

  if (!fullPath.startsWith(PUBLIC_DIR) || !fs.existsSync(fullPath) || fs.statSync(fullPath).isDirectory()) {
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("Not found");
    return;
  }

  const ext = path.extname(fullPath);
  res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream" });
  fs.createReadStream(fullPath).pipe(res);
});

const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false });

server.on("upgrade", (req, socket, head) => {
  if ("setNoDelay" in socket && typeof socket.setNoDelay === "function") {
    socket.setNoDelay(true);
  }
  const url = new URL(req.url || "/", `http://${req.headers.host || "localhost"}`);
  const role = url.pathname === "/ws/device" ? "device"
    : url.pathname === "/ws/client" ? "client"
      : null;

  if (!role) {
    socket.write("HTTP/1.1 404 Not Found\r\n\r\n");
    socket.destroy();
    return;
  }

  const token = url.searchParams.get("token") || "";
  if (AUTH_TOKEN && token !== AUTH_TOKEN) {
    socket.write("HTTP/1.1 401 Unauthorized\r\n\r\n");
    socket.destroy();
    return;
  }

  const roomId = (url.searchParams.get("room") || "badge1").slice(0, 32);

  wss.handleUpgrade(req, socket, head, (ws) => {
    wss.emit("connection", ws, role, roomId);
  });
});

wss.on("connection", (ws: WebSocket, role: "device" | "client", roomId: string) => {
  const room = getRoom(roomId);

  if (role === "device") {
    if (room.device) {
      room.device.terminate();
    }
    room.device = ws;
    console.log(`[room:${roomId}] badge device connected`);
    broadcastJson(room.clients, { type: "device", online: true });

    ws.on("error", (err) => {
      console.warn(`[room:${roomId}] badge device socket error:`, err.message);
    });

    ws.on("message", (data: RawData, isBinary: boolean) => {
      if (isBinary) return;
      const text = data.toString();
      try {
        const parsed = JSON.parse(text) as Record<string, unknown>;
        if (parsed.type === "status") room.lastStatus = parsed;
        broadcastJson(room.clients, parsed);
      } catch {
        // Ignore malformed text frames from device
      }
    });

    ws.on("close", () => {
      if (room.device === ws) {
        room.device = null;
        room.lastStatus = null;
        room.inFlight = false;
        room.lastButtons = 0;
        resetPendingMouse(room);
        console.log(`[room:${roomId}] badge device disconnected`);
        broadcastJson(room.clients, { type: "device", online: false });
      }
    });
    return;
  }

  // Frontend client connection
  room.clients.add(ws);
  console.log(`[room:${roomId}] web client connected (total ${room.clients.size})`);
  ws.send(JSON.stringify({ type: "device", online: room.device !== null }));
  if (room.lastStatus) ws.send(JSON.stringify(room.lastStatus));

  ws.on("error", (err) => {
    console.warn(`[room:${roomId}] web client socket error:`, err.message);
  });

  ws.on("message", (data: RawData, isBinary: boolean) => {
    const dev = room.device;
    if (!dev || dev.readyState !== WebSocket.OPEN) return;

    if (isBinary) {
      const buf = Buffer.isBuffer(data) ? data : Buffer.from(data as ArrayBuffer);
      if (!isValidHidFrame(buf)) return;

      if (buf[0] === OP_MOUSE) {
        const buttons = buf[1] & 0x07;
        const dx = buf.readInt16LE(2);
        const dy = buf.readInt16LE(4);
        const wh = buf.readInt8(6);

        /* Coalesce pure motion/wheel when a previous frame is still draining
         * over Wi-Fi/TCP so stale frames never pile up in the send buffer. */
        if ((room.inFlight || dev.bufferedAmount > 0) && buttons === room.lastButtons) {
          room.pendDx += dx;
          room.pendDy += dy;
          room.pendWheel += wh;
          room.pendButtons = buttons;
          room.hasPendingMouse = true;
          return;
        }

        if (room.hasPendingMouse) {
          const merged = Buffer.allocUnsafe(7);
          merged[0] = OP_MOUSE;
          merged[1] = buttons;
          merged.writeInt16LE(clamp(room.pendDx + dx, -2048, 2048), 2);
          merged.writeInt16LE(clamp(room.pendDy + dy, -2048, 2048), 4);
          merged.writeInt8(clamp(room.pendWheel + wh, -127, 127), 6);
          resetPendingMouse(room);
          room.lastButtons = buttons;
          sendDeviceBinary(room, merged);
        } else {
          room.lastButtons = buttons;
          sendDeviceBinary(room, buf);
        }
        return;
      }

      if (buf[0] === OP_RELEASE) {
        resetPendingMouse(room);
        room.lastButtons = 0;
      } else if (room.hasPendingMouse) {
        flushPendingMouse(room);
      }
      sendDeviceBinary(room, buf);
    } else {
      const text = data.toString();
      dev.send(text);
    }
  });

  ws.on("close", () => {
    room.clients.delete(ws);
    console.log(`[room:${roomId}] web client disconnected (remaining ${room.clients.size})`);
    // Safety: immediately release all keys & mouse buttons when the controller disconnects.
    if (room.device && room.device.readyState === WebSocket.OPEN) {
      room.device.send(RELEASE_ALL_FRAME, { binary: true });
    }
  });
});

server.listen(PORT, HOST, () => {
  const ips = getLanIps();
  console.log(`Super Mouse server listening on http://${HOST}:${PORT}`);
  console.log(`  Web UI:     http://localhost:${PORT}`);
  for (const ip of ips) {
    console.log(`  Badge IP:   ${ip}:${PORT}  (auto-expands to ws://${ip}:${PORT}/ws/device?room=badge1)`);
  }
  if (AUTH_TOKEN) console.log("  Auth token: enabled (SUPER_MOUSE_TOKEN)");
});
