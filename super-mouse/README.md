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
