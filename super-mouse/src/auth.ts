// Password login for exposing the page through a Cloudflare Tunnel.
// Every login attempt is appended to data/login-attempts.jsonl with all the
// identifying data the request (and the login page's fingerprint script) carries.
// Lockouts are rebuilt from that log on boot, so restarting does not reset them.
import crypto from "node:crypto";
import dns from "node:dns/promises";
import fs from "node:fs";
import path from "node:path";
import type http from "node:http";

export const DATA_DIR = path.resolve(process.env.SM_DATA_DIR || path.join(import.meta.dirname, "../data"));
const LOG_FILE = path.join(DATA_DIR, "login-attempts.jsonl");
const AUTH_FILE = path.join(DATA_DIR, "auth.json");

const SESSION_MS = Number(process.env.SM_SESSION_HOURS || 12) * 3600_000;
const COOKIE = "sm_session";

// Per-IP backoff: FREE_FAILS mistakes, then BASE_LOCK_MS doubling each further failure.
const FREE_FAILS = 3;
const BASE_LOCK_MS = 30_000;
const MAX_LOCK_MS = 24 * 3600_000;
const FORGET_AFTER_MS = 24 * 3600_000; // count resets this long after the lock ends

// Global backoff against many IPs guessing at once. It also locks out the owner,
// so it is milder and capped at an hour. SM_GLOBAL_LOCK=0 disables it.
const GLOBAL_ENABLED = process.env.SM_GLOBAL_LOCK !== "0";
const GLOBAL_WINDOW_MS = 3600_000;
const GLOBAL_FREE_FAILS = 20;
const GLOBAL_BASE_LOCK_MS = 60_000;
const GLOBAL_MAX_LOCK_MS = 3600_000;

const KEEP_IN_MEMORY = 1000;
export const PUBLIC_LOG = process.env.SM_PUBLIC_LOG !== "0";

export type Result = "success" | "failure" | "locked" | "logout";

export interface Attempt {
  id: string;
  time: string;
  result: Result;
  reason?: string;
  lockedUntil?: string;
  failCount?: number;
  ip: string;
  ipSource: string;
  socketIp: string;
  forwardedFor?: string;
  rdns?: string[];
  geo: Record<string, string>;
  cf: Record<string, string>;
  userAgent?: string;
  clientHints: Record<string, string>;
  acceptLanguage?: string;
  referer?: string;
  origin?: string;
  method: string;
  url: string;
  httpVersion: string;
  headers: Record<string, string>;
  client?: Record<string, unknown>;
  fingerprint?: string;
  usernameTried?: string;
  passwordLength?: number;
}

interface IpState { fails: number; lastFail: number; lockedUntil: number }

const attempts: Attempt[] = [];
const ipState = new Map<string, IpState>();
const globalFails: number[] = [];
const sessions = new Map<string, { ip: string; expires: number }>();

// ---------- password ----------

interface PwHash { salt: string; hash: string }
let pw: PwHash;

function scrypt(password: string, salt: Buffer): Promise<Buffer> {
  return new Promise((resolve, reject) =>
    crypto.scrypt(password, salt, 64, { N: 16384, r: 8, p: 1 }, (err, key) => (err ? reject(err) : resolve(key))),
  );
}

async function makeHash(password: string): Promise<PwHash> {
  const salt = crypto.randomBytes(16);
  return { salt: salt.toString("hex"), hash: (await scrypt(password, salt)).toString("hex") };
}

async function checkPassword(candidate: string): Promise<boolean> {
  const got = await scrypt(candidate, Buffer.from(pw.salt, "hex"));
  return crypto.timingSafeEqual(got, Buffer.from(pw.hash, "hex"));
}

export async function setPassword(password: string): Promise<void> {
  fs.mkdirSync(DATA_DIR, { recursive: true, mode: 0o700 });
  fs.writeFileSync(AUTH_FILE, JSON.stringify(await makeHash(password)) + "\n", { mode: 0o600 });
}

// ---------- boot ----------

export async function initAuth(): Promise<void> {
  fs.mkdirSync(DATA_DIR, { recursive: true, mode: 0o700 });
  if (process.env.SM_PASSWORD) {
    if (process.env.SM_PASSWORD.length < 8) throw new Error("SM_PASSWORD must be at least 8 characters");
    pw = await makeHash(process.env.SM_PASSWORD);
  } else if (fs.existsSync(AUTH_FILE)) {
    pw = JSON.parse(fs.readFileSync(AUTH_FILE, "utf8"));
  } else {
    const generated = crypto.randomBytes(15).toString("base64url");
    await setPassword(generated);
    pw = JSON.parse(fs.readFileSync(AUTH_FILE, "utf8"));
    console.log(`\n  No password set. Generated one (shown once, hash stored in ${AUTH_FILE}):\n\n      ${generated}\n\n  Change it with: npm run set-password\n`);
  }

  if (fs.existsSync(LOG_FILE)) {
    const byId = new Map<string, number>();
    for (const line of fs.readFileSync(LOG_FILE, "utf8").split("\n")) {
      if (!line.trim()) continue;
      try {
        const a: Attempt = JSON.parse(line);
        // A later line with the same id is the reverse-DNS update for that attempt.
        const i = byId.get(a.id);
        if (i !== undefined) { attempts[i] = a; continue; }
        byId.set(a.id, attempts.push(a) - 1);
        replay(a);
      } catch { /* torn last line after a crash */ }
    }
    attempts.splice(0, Math.max(0, attempts.length - KEEP_IN_MEMORY));
  }
}

function replay(a: Attempt): void {
  const t = Date.parse(a.time);
  if (a.result === "failure") noteFailure(a.ip, t);
  else if (a.result === "success") ipState.delete(a.ip);
}

// ---------- backoff ----------

function lockFor(fails: number, free: number, base: number, max: number): number {
  if (fails <= free) return 0;
  return Math.min(max, base * 2 ** Math.min(30, fails - free - 1));
}

function currentIpState(ip: string, now: number): IpState | undefined {
  const s = ipState.get(ip);
  if (s && now > Math.max(s.lockedUntil, s.lastFail) + FORGET_AFTER_MS) {
    ipState.delete(ip);
    return undefined;
  }
  return s;
}

function noteFailure(ip: string, now: number): IpState {
  const s = currentIpState(ip, now) || { fails: 0, lastFail: 0, lockedUntil: 0 };
  s.fails++;
  s.lastFail = now;
  s.lockedUntil = now + lockFor(s.fails, FREE_FAILS, BASE_LOCK_MS, MAX_LOCK_MS);
  ipState.set(ip, s);
  globalFails.push(now);
  return s;
}

function globalLockedUntil(now: number): number {
  if (!GLOBAL_ENABLED) return 0;
  while (globalFails.length && globalFails[0] < now - GLOBAL_WINDOW_MS) globalFails.shift();
  const n = globalFails.length;
  if (!n) return 0;
  return globalFails[n - 1] + lockFor(n, GLOBAL_FREE_FAILS, GLOBAL_BASE_LOCK_MS, GLOBAL_MAX_LOCK_MS);
}

export function lockStatus(ip: string, now = Date.now()) {
  const s = currentIpState(ip, now);
  const ipUntil = s && s.lockedUntil > now ? s.lockedUntil : 0;
  const gUntil = globalLockedUntil(now);
  const until = Math.max(ipUntil, gUntil > now ? gUntil : 0);
  return {
    locked: until > now,
    lockedUntil: until > now ? new Date(until).toISOString() : null,
    retryAfterSec: until > now ? Math.ceil((until - now) / 1000) : 0,
    scope: until === 0 ? null : until === ipUntil ? "ip" : "global",
    failCount: s?.fails || 0,
    freeAttemptsLeft: Math.max(0, FREE_FAILS - (s?.fails || 0)),
    nextLockSec: Math.round(lockFor((s?.fails || 0) + 1, FREE_FAILS, BASE_LOCK_MS, MAX_LOCK_MS) / 1000),
  };
}

// ---------- request identity ----------

function first(v: string | string[] | undefined): string | undefined {
  return Array.isArray(v) ? v[0] : v;
}

function isLoopback(ip: string): boolean {
  return ip === "127.0.0.1" || ip === "::1" || ip === "::ffff:127.0.0.1";
}

// Proxy headers are only trusted when the TCP peer is local (cloudflared runs on this machine).
export function clientIp(req: http.IncomingMessage): { ip: string; source: string } {
  const sock = req.socket.remoteAddress || "unknown";
  if (isLoopback(sock)) {
    const cf = first(req.headers["cf-connecting-ip"]);
    if (cf) return { ip: cf.trim(), source: "cf-connecting-ip" };
    const tci = first(req.headers["true-client-ip"]);
    if (tci) return { ip: tci.trim(), source: "true-client-ip" };
    const xff = first(req.headers["x-forwarded-for"]);
    if (xff) return { ip: xff.split(",")[0].trim(), source: "x-forwarded-for" };
  }
  return { ip: sock, source: "socket" };
}

export function isHttps(req: http.IncomingMessage): boolean {
  if (first(req.headers["x-forwarded-proto"]) === "https") return true;
  return /"scheme"\s*:\s*"https"/.test(first(req.headers["cf-visitor"]) || "");
}

const SECRET_HEADERS = new Set(["cookie", "authorization", "proxy-authorization", "cf-access-jwt-assertion"]);
const GEO_HEADERS: Record<string, string> = {
  "cf-ipcountry": "country", "cf-ipcontinent": "continent", "cf-ipcity": "city", "cf-region": "region",
  "cf-region-code": "regionCode", "cf-postal-code": "postalCode", "cf-metro-code": "metroCode",
  "cf-iplatitude": "latitude", "cf-iplongitude": "longitude", "cf-timezone": "timezone",
};

function cut(s: unknown, n = 512): string {
  return String(s).slice(0, n);
}

function describe(req: http.IncomingMessage): Omit<Attempt, "id" | "time" | "result"> {
  const { ip, source } = clientIp(req);
  const headers: Record<string, string> = {};
  const geo: Record<string, string> = {};
  const cf: Record<string, string> = {};
  const clientHints: Record<string, string> = {};
  for (const [k, v] of Object.entries(req.headers)) {
    if (v === undefined) continue;
    const val = cut(Array.isArray(v) ? v.join(", ") : v, 2048);
    if (SECRET_HEADERS.has(k)) { headers[k] = "[redacted]"; continue; }
    headers[k] = val;
    if (GEO_HEADERS[k]) geo[GEO_HEADERS[k]] = val;
    else if (k.startsWith("cf-")) cf[k] = val;
    if (k.startsWith("sec-ch-")) clientHints[k] = val;
  }
  return {
    ip, ipSource: source,
    socketIp: req.socket.remoteAddress || "unknown",
    forwardedFor: first(req.headers["x-forwarded-for"]),
    geo, cf, clientHints, headers,
    userAgent: first(req.headers["user-agent"]),
    acceptLanguage: first(req.headers["accept-language"]),
    referer: first(req.headers["referer"]),
    origin: first(req.headers["origin"]),
    method: req.method || "", url: cut(req.url || ""), httpVersion: req.httpVersion,
  };
}

// Only plain JSON from the fingerprint script; bounded so a client can't bloat the log.
function sanitizeClient(v: unknown, depth = 0): unknown {
  if (v === null || typeof v === "boolean" || typeof v === "number") return v;
  if (typeof v === "string") return cut(v, 1024);
  if (depth > 3) return undefined;
  if (Array.isArray(v)) return v.slice(0, 32).map((x) => sanitizeClient(x, depth + 1));
  if (typeof v === "object") {
    const out: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v as object).slice(0, 64)) out[cut(k, 64)] = sanitizeClient(x, depth + 1);
    return out;
  }
  return undefined;
}

function record(req: http.IncomingMessage, result: Result, extra: Partial<Attempt>): Attempt {
  const a: Attempt = { id: crypto.randomUUID(), time: new Date().toISOString(), result, ...describe(req), ...extra };
  attempts.push(a);
  if (attempts.length > KEEP_IN_MEMORY) attempts.shift();
  fs.appendFileSync(LOG_FILE, JSON.stringify(a) + "\n", { mode: 0o600 });
  const where = [a.geo.city, a.geo.region, a.geo.country].filter(Boolean).join(", ");
  console.log(`[auth] ${a.time} ${result.toUpperCase()} ip=${a.ip}${where ? ` (${where})` : ""} ua="${a.userAgent || ""}"${a.reason ? ` reason=${a.reason}` : ""}`);
  // Reverse DNS runs after the response; the update is appended under the same id.
  dns.reverse(a.ip).then((names) => {
    if (!names.length) return;
    a.rdns = names.slice(0, 5);
    fs.appendFileSync(LOG_FILE, JSON.stringify(a) + "\n");
  }).catch(() => {});
  return a;
}

export function recentAttempts(limit = 200): Attempt[] {
  return attempts.slice(-limit).reverse();
}

// ---------- sessions ----------

function parseCookies(req: http.IncomingMessage): Record<string, string> {
  const out: Record<string, string> = {};
  for (const part of (req.headers.cookie || "").split(";")) {
    const i = part.indexOf("=");
    if (i > 0) out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
  }
  return out;
}

export function isAuthed(req: http.IncomingMessage): boolean {
  const token = parseCookies(req)[COOKIE];
  if (!token) return false;
  const s = sessions.get(token);
  if (!s) return false;
  if (s.expires < Date.now()) { sessions.delete(token); return false; }
  return true;
}

function cookie(req: http.IncomingMessage, value: string, maxAgeSec: number): string {
  return `${COOKIE}=${value}; Path=/; HttpOnly; SameSite=Strict; Max-Age=${maxAgeSec}${isHttps(req) ? "; Secure" : ""}`;
}

// ---------- handlers ----------

function json(res: http.ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}): void {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store", ...headers });
  res.end(JSON.stringify(body));
}

function readBody(req: http.IncomingMessage, max = 32 * 1024): Promise<string> {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks: Buffer[] = [];
    req.on("data", (c: Buffer) => {
      size += c.length;
      if (size > max) { reject(new Error("body too large")); req.destroy(); return; }
      chunks.push(c);
    });
    req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    req.on("error", reject);
  });
}

// Blocks cross-site form posts; SameSite=Strict covers the session cookie itself.
function sameOrigin(req: http.IncomingMessage): boolean {
  const origin = first(req.headers.origin);
  if (!origin) return true;
  try { return new URL(origin).host === req.headers.host; } catch { return false; }
}

async function handleLogin(req: http.IncomingMessage, res: http.ServerResponse): Promise<void> {
  if (!sameOrigin(req)) { json(res, 403, { error: "bad origin" }); return; }
  let body: { password?: unknown; username?: unknown; client?: unknown } = {};
  try { body = JSON.parse(await readBody(req)); } catch { /* recorded as malformed below */ }
  const client = sanitizeClient(body.client) as Record<string, unknown> | undefined;
  const fingerprint = typeof client?.fingerprint === "string" ? client.fingerprint : undefined;
  const password = typeof body.password === "string" ? body.password : "";
  const extra: Partial<Attempt> = {
    client, fingerprint,
    usernameTried: typeof body.username === "string" && body.username ? cut(body.username, 128) : undefined,
    passwordLength: password.length,
  };

  const { ip } = clientIp(req);
  const before = lockStatus(ip);
  if (before.locked) {
    // Not evaluated at all while locked, so a correct guess here reveals nothing.
    record(req, "locked", { ...extra, reason: `${before.scope} lockout`, lockedUntil: before.lockedUntil!, failCount: before.failCount });
    json(res, 429, { error: "locked", ...before }, { "Retry-After": String(before.retryAfterSec) });
    return;
  }

  const ok = password.length > 0 && password.length <= 1024 && (await checkPassword(password));
  if (!ok) {
    const s = noteFailure(ip, Date.now());
    const after = lockStatus(ip);
    record(req, "failure", { ...extra, reason: password ? "wrong password" : "empty/malformed", failCount: s.fails, lockedUntil: after.lockedUntil || undefined });
    json(res, 401, { error: "wrong password", ...after }, after.locked ? { "Retry-After": String(after.retryAfterSec) } : {});
    return;
  }

  ipState.delete(ip);
  const token = crypto.randomBytes(32).toString("base64url");
  sessions.set(token, { ip, expires: Date.now() + SESSION_MS });
  record(req, "success", extra);
  json(res, 200, { ok: true }, { "Set-Cookie": cookie(req, token, Math.floor(SESSION_MS / 1000)) });
}

function handleLogout(req: http.IncomingMessage, res: http.ServerResponse): void {
  if (!sameOrigin(req)) { json(res, 403, { error: "bad origin" }); return; }
  const token = parseCookies(req)[COOKIE];
  if (token && sessions.delete(token)) record(req, "logout", {});
  json(res, 200, { ok: true }, { "Set-Cookie": cookie(req, "", 0) });
}

/** Returns true when it handled the request. */
export async function handleAuthRoute(req: http.IncomingMessage, res: http.ServerResponse, pathname: string): Promise<boolean> {
  if (pathname === "/auth/login" && req.method === "POST") { await handleLogin(req, res); return true; }
  if (pathname === "/auth/logout" && req.method === "POST") { handleLogout(req, res); return true; }
  if (pathname === "/auth/status" && req.method === "GET") {
    const d = describe(req);
    json(res, 200, { authed: isAuthed(req), publicLog: PUBLIC_LOG, ...lockStatus(d.ip), you: { ip: d.ip, geo: d.geo, userAgent: d.userAgent } });
    return true;
  }
  if (pathname === "/auth/attempts" && req.method === "GET") {
    if (!PUBLIC_LOG && !isAuthed(req)) { json(res, 401, { error: "sign in to view" }); return true; }
    json(res, 200, { attempts: recentAttempts() });
    return true;
  }
  return false;
}
