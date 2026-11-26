# Super Mouse — Agent Context

Vanilla ES-module web app (+ a tiny TypeScript static server) for The Badge's **Super Mouse** app.
The computer the badge is plugged into over USB is the **source**: the page writes HID frames to the
badge's USB-Serial/JTAG port with WebSerial. The badge's paired **Bluetooth HID host is the only target**.
No Wi-Fi is used.

## Directory Map
- `src/server.ts` — Static server for `public/`, bound to `127.0.0.1:8765` (WebSerial needs a secure context; `http://localhost` qualifies).
- `public/index.html`, `public/style.css` — Control UI (status pills, Pointer Lock trackpad, App Switcher, Sticky Modifiers, Quick Combos, Type Text, connect help).
- `public/hid-map.js` — DOM `KeyboardEvent.code` and ASCII → USB HID Usage Page `0x07` keycodes + modifier bitmasks.
- `public/app.js` — WebSerial link (connect/heartbeat/status parsing), mouse delta coalescing (15 ms, BLE interval), 6KRO keyboard state machine, App Switcher (`switcherMod`), Sticky Modifiers, and safety `releaseAll()` hooks.

## Commands
```sh
npm install
npm run build        # compiles src/*.ts -> dist/*.js
npm start            # serves http://localhost:8765 (static files in public/ reload on browser refresh)
node --check public/app.js
```

## Firmware Pairing (Parent Repo `../`)
- USB source: `../main/super_mouse_usb.c` (ESP-only, listed in `../sim/build.py` `ESP_ONLY`).
- Firmware UI: `../main/apps/app_super_mouse.c` (Target = BLE host pill → host picker; Source = USB card).
- BLE HID 6KRO API: `port_hid_keyboard_report()` & `port_hid_release_all()` in `../main/ble/hid_mouse.c` (reuses Report ID 1 mouse & Report ID 2 6KRO keyboard — do **not** change the HID report map or existing Bluetooth pairings break).

## Serial Protocol
Host → badge, every frame prefixed with sync byte `0xA5`:
| Opcode | Name | Payload after sync | Size |
|---|---|---|---|
| `0x01` | Mouse | `[0x01, buttons_u8, dx_i16_le, dy_i16_le, wheel_i8]` | 7 B |
| `0x02` | Keyboard 6KRO | `[0x02, modifiers_u8, k0..k5]` | 8 B |
| `0x03` | Macro Tap | `[0x03, modifiers_u8, k0..k2, hold_ms_u8]` | 6 B |
| `0x04` | Release All | `[0x04]` | 1 B |
| `0x05` | Heartbeat | `[0x05]` (sent every 1 s) | 1 B |

Badge → host: one console line per second (and on BLE/live changes), mixed with ESP_LOG output:
`@SM {"live":true,"ble":true,"peer":"...","batt":87,"rx":123}`

## Key Invariants & UX Decisions
1. **Frames stay in sync:** any opcode/layout change in `public/app.js` MUST match `../main/super_mouse_usb.c` (`frame_len`, `dispatch`).
2. **No stale replay:** the page only sends input while it has seen an `@SM` line in the last 3 s (`badgeLive()`); otherwise frames would sit in the USB pipe while the badge app is closed and replay later. The badge also drains the RX FIFO on app entry.
3. **Never leave keys stuck:** `releaseAll()` (`0x04`) fires on `blur`, `visibilitychange`, Pointer Lock exit and Disconnect; the badge releases all on its own when no frame/heartbeat arrives for 3 s.
4. **One serial owner:** `idf.py monitor` (or any terminal) holding the port blocks the page. Unverified on hardware: whether Chrome opening the port toggles DTR/RTS (the USJ resets the chip on RTS with DTR low). If connecting reboots the badge, set the signals explicitly after `open()`.
5. **App Switcher (`Cmd+Tab (Hold)` / `Alt+Tab (Hold)`):** `switcherMod` is held after the 35 ms `Tab` tap, survives Click 1 (entering Pointer Lock on `trackpad`), and only releases on `mouseup` after a `mousedown` that started **inside** Pointer Lock (`capturedMouseDown`), or on explicit mouse-button click / `Release All`.
6. **Scroll direction:** Default wheel mapping is `e.deltaY > 0 ? 1 : -1` (natural/reversed), flipped by the `Invert Scroll` checkbox.
7. **Smoothness history:** read `LATENCY-LOG.md` before working on cursor smoothness. It covers the Wi-Fi/BLE coexistence gaps that motivated the USB source.
