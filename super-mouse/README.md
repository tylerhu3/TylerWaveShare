# Super Mouse — Web Controller

Web controller for **The Badge** Super Mouse app (`main/apps/app_super_mouse.c` + `main/super_mouse_usb.c`).
Plug the badge into a computer with USB: that computer runs this page and becomes the **source**. The page
captures mouse movement, clicks, scroll wheel and multi-key (6KRO) keyboard chords and writes them to the
badge over USB serial (WebSerial). The badge forwards them to its paired **Bluetooth** HID host, the only target.

```
[computer + this page] --USB--> [badge] --BLE HID--> [target: Mac / PC / iPad / phone]
```

Wi-Fi is not used, so the 100–320 ms Wi-Fi/BLE coexistence gaps (see `LATENCY-LOG.md`) are gone.

## Quick Start

```bash
npm install
npm run build
npm start
```

1. Pair the badge with the target over Bluetooth (any app that uses BLE, e.g. Air Mouse, or the badge's host picker).
2. Plug the badge into this computer with USB and open **Super Mouse** on the badge.
3. Open `http://localhost:8765` in **Chrome or Edge** (WebSerial), click **Connect Badge (USB)** and pick the
   `USB JTAG/serial debug unit` port. The badge shows **Web app live**.

Tap the **target** pill on the badge to switch between paired Bluetooth hosts.

## Remote Access (Cloudflare Tunnel) & Login

Every page except `/login` needs a password. The server still binds to `127.0.0.1`; point the tunnel at it:

```bash
SM_PASSWORD='a long passphrase' npm start      # or: npm run set-password (stores a scrypt hash in data/auth.json)
cloudflared tunnel --url http://localhost:8765
```

With no `SM_PASSWORD` and no `data/auth.json`, the first start generates a password and prints it once.

- **Attempt log:** every sign-in (success, wrong password, blocked, sign-out) is appended to
  `data/login-attempts.jsonl` with IP (`CF-Connecting-IP`), Cloudflare geo headers, reverse DNS, every request header
  (cookies redacted), and a browser fingerprint from the login page (timezone, languages, screen, GPU, CPU/RAM, canvas
  hash, UA client hints, battery). Passwords tried are never stored, only their length. The log is shown on `/login`
  (and via **Access Log** in the controller header).
- **Public by default:** anyone who opens `/login` sees the table, including your own sign-ins and IP.
  `SM_PUBLIC_LOG=0` shows it only after signing in.
- **City / lat-long:** turn on *Add visitor location headers* (Cloudflare dashboard → Rules → Managed Transforms).
  Without it you only get `CF-IPCountry`.
- **Backoff:** 3 free wrong passwords per IP, then a lockout of 30 s that doubles with each further failure (capped at
  24 h); attempts while locked are rejected without checking the password. Across all IPs, more than 20 failures in an
  hour starts a global lockout of 1 min doubling up to 1 h (this also blocks you; `SM_GLOBAL_LOCK=0` disables it).
  Lockouts are rebuilt from the log on restart.
- Sessions are in memory (`SM_SESSION_HOURS`, default 12) and end when the server restarts.

## Web UI Features
- **Pointer Lock Capture:** Click the trackpad to capture raw mouse movement, Left/Right/Middle clicks, scroll wheel (natural/reversed default + **Invert Scroll** toggle), and 6KRO keyboard input. Press `Esc` to unlock.
- **App Switcher (`Cmd + Tab (Hold)` / `Alt + Tab (Hold)`):** Presses `Cmd+Tab` (or `Alt+Tab`), releases `Tab` after 35 ms, and keeps `Cmd`/`Alt` held until you click inside the captured area (or click **Release All**).
- **Multi-Key & OS Shortcuts:** **CapsLock → Cmd** remap, Sticky Modifiers (click to arm, double-click to lock), and one-click Quick Combos.
- **Status pills:** `USB` (port open), `Badge` (Super Mouse app answering), `BLE: <peer>`, `Batt: %`.

## Serial Protocol
See `AGENTS.md` (frames are prefixed with `0xA5`; the badge answers with `@SM {json}` status lines).

## Troubleshooting
- **"Could not open the badge":** another program holds the port. Quit `idf.py monitor`, `screen`, Arduino serial monitor, etc.
- **Badge pill says "open Super Mouse":** the port is open but the badge app isn't running (or the badge is on another screen).
- **BLE: advertising:** the badge isn't connected to a Bluetooth host. Tap the target pill on the badge to pick one, or pair it from the host's Bluetooth settings.
