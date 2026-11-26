// Host for the Super Mouse web app. This server owns the badge's USB serial
// port (badge-link.ts) and every signed-in page shares it over /ws, locally or
// through a Cloudflare Tunnel (cloudflared -> 127.0.0.1:PORT).
// Everything except the login page needs a session (see auth.ts).
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { handleAuthRoute, initAuth, isAuthed, PUBLIC_LOG, sameOrigin } from "./auth.js";
import { attachBadgeLink } from "./badge-link.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PUBLIC_DIR = path.resolve(__dirname, "../public");

const PORT = Number(process.env.PORT || 8765);
const HOST = process.env.HOST || "127.0.0.1";

// Served without a session: the login page and what it needs to render.
const OPEN_PATHS = new Set(["/login", "/login.html", "/login.js", "/style.css", "/icon.svg", "/favicon.ico"]);

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

const SECURITY_HEADERS = {
  "X-Frame-Options": "DENY",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "same-origin",
};

function serveFile(res: http.ServerResponse, rel: string): void {
  const fullPath = path.join(PUBLIC_DIR, path.normalize(rel).replace(/^(\.\.[/\\])+/, ""));
  if (!fullPath.startsWith(PUBLIC_DIR + path.sep) || !fs.existsSync(fullPath) || fs.statSync(fullPath).isDirectory()) {
    res.writeHead(404, { "Content-Type": "text/plain", ...SECURITY_HEADERS });
    res.end("Not found");
    return;
  }
  const ext = path.extname(fullPath);
  res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream", "Cache-Control": "no-cache", ...SECURITY_HEADERS });
  fs.createReadStream(fullPath).pipe(res);
}

async function route(req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
  const url = new URL(req.url || "/", "http://localhost");
  const p = url.pathname;

  if (p.startsWith("/auth/")) {
    if (!(await handleAuthRoute(req, res, p))) { res.writeHead(404, SECURITY_HEADERS); res.end(); }
    return;
  }
  if (req.method !== "GET" && req.method !== "HEAD") { res.writeHead(405, SECURITY_HEADERS); res.end(); return; }
  if (p === "/login") { serveFile(res, "/login.html"); return; }
  if (OPEN_PATHS.has(p)) { serveFile(res, p); return; }

  if (!isAuthed(req)) {
    if (p === "/" || p.endsWith(".html")) {
      res.writeHead(302, { Location: "/login", "Cache-Control": "no-store", ...SECURITY_HEADERS });
    } else {
      res.writeHead(401, { "Content-Type": "text/plain", ...SECURITY_HEADERS });
    }
    res.end(p === "/" ? "" : "Sign in required");
    return;
  }
  serveFile(res, p === "/" ? "/index.html" : p);
}

await initAuth();

const server = http.createServer((req, res) => {
  route(req, res).catch((err) => {
    console.error("[server]", err);
    if (!res.headersSent) res.writeHead(500, SECURITY_HEADERS);
    res.end();
  });
});

// Browsers always send Origin on a WebSocket upgrade; require it to match so a
// foreign page cannot drive the badge with a visitor's cookie.
attachBadgeLink(server, (req) => !!req.headers.origin && sameOrigin(req) && isAuthed(req));

server.listen(PORT, HOST, () => {
  console.log(`Super Mouse web app: http://localhost:${PORT}  (badge USB link is shared by every signed-in browser)`);
  console.log(`Login attempts log is ${PUBLIC_LOG ? "PUBLIC on /login (SM_PUBLIC_LOG=0 to hide it until signed in)" : "visible only when signed in"}.`);
});
