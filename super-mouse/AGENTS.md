# Super Mouse — Agent Context

Node.js (TypeScript) WebSocket relay + vanilla ES-module web frontend for The Badge's **Super Mouse** app.

## Directory Map
- `src/protocol.ts` — Binary HID frame opcodes (`0x01`–`0x04`), length validation, and `RELEASE_ALL_FRAME`.
- `src/server.ts` — HTTP static server (`public/`), `/api/info` LAN IP endpoint, and WS relay (`/ws/device`, `/ws/client`) with room routing and disconnect auto-release (`0x04`).
- `public/index.html`, `public/style.css` — Control UI (status bar, Pointer Lock trackpad, App Switcher, Sticky Modifiers, Quick Combos, Type Text, Badge URL helper).
- `public/hid-map.js` — DOM `KeyboardEvent.code` and ASCII → USB HID Usage Page `0x07` keycodes + modifier bitmasks.
- `public/app.js` — Client WS loop, 40 Hz mouse delta coalescing, 6KRO keyboard state machine, App Switcher (`switcherMod`), Sticky Modifiers, and safety `releaseAll()` hooks.

## Commands
```sh
npm install
npm run build        # compiles src/*.ts -> dist/*.js
npm start            # serves http://0.0.0.0:8765 (static files in public/ reload on browser refresh)
node --check public/app.js
```

## Firmware Pairing (Parent Repo `../`)
- Firmware bridge: `../main/super_mouse_net.c` (ESP-only, listed in `../sim/build.py` `ESP_ONLY`).
- Firmware UI: `../main/apps/app_super_mouse.c` + icon `../main/assets/app_icon_super_mouse.c`.
- BLE HID 6KRO API: `port_hid_keyboard_report()` & `port_hid_release_all()` in `../main/ble/hid_mouse.c` (reuses Report ID 1 mouse & Report ID 2 6KRO keyboard — do **not** change the HID report map or existing Bluetooth pairings break).
- Firmware flash command on this Mac:
  ```sh
  source ~/.espressif/tools/activate_idf_v6.0.2.sh && idf.py -p /dev/cu.usbmodem* flash
  ```

## Key Invariants & UX Decisions
1. **Binary frames stay in sync:** Any opcode/layout change in `src/protocol.ts` or `public/app.js` MUST match `../main/super_mouse_net.c` (`handle_mouse_frame`, `handle_kbd_frame`, `handle_macro_frame`).
2. **Never leave keys stuck:** `releaseAll()` (`0x04`) fires on `blur`, `visibilitychange`, Pointer Lock exit, and backend client disconnect.
3. **App Switcher (`Cmd+Tab (Hold)` / `Alt+Tab (Hold)`):** `switcherMod` is held after the 35 ms `Tab` tap, survives Click 1 (entering Pointer Lock on `trackpad`), and only releases on `mouseup` after a `mousedown` that started **inside** Pointer Lock (`capturedMouseDown`), or on explicit mouse-button click / `Release All`.
4. **Scroll direction:** Default wheel mapping is `e.deltaY > 0 ? 1 : -1` (natural/reversed), flipped by the `Invert Scroll` checkbox.
5. **URL shorthand:** `expand_ws_url()` in `../main/super_mouse_net.c` expands `IP` or `IP:port` to `ws://<ip>:<port>/ws/device?room=badge1` (default port `8765`).
6. **ESP32-S3 RAM headroom:** Simultaneous Wi-Fi + BLE leaves ~9 KB internal SRAM free; keep `esp_websocket_client` `buffer_size` small (512) and `task_stack` <= 3072.
