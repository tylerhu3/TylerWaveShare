# Super Mouse — Backend & Web Controller

Standalone relay server + web frontend for **The Badge** Super Mouse app (`main/apps/app_super_mouse.c` + `main/super_mouse_net.c`).
Captures mouse movement, clicks, scroll wheel, and multi-key (6KRO) keyboard chords in the browser and relays them over WebSocket to the Waveshare ESP32-S3 badge, which forwards them to its paired Bluetooth HID host.

## Quick Start

```bash
npm install
npm run build
npm start
```

Open `http://localhost:8765` in your browser. Enter the displayed LAN **`IP:port`** (e.g., `192.168.2.1:8765` or just `192.168.2.1`) in the badge's **Super Mouse → Server** button (or set `BADGE_SUPER_MOUSE_URL` in `main/secrets.h`). The badge automatically expands `IP` / `IP:port` to `ws://<ip>:<port>/ws/device?room=badge1`.

### Optional Auth Token
```bash
SUPER_MOUSE_TOKEN=mysecret npm start
```
When using a token, custom room, or `wss://`, enter the full WebSocket URL on the badge.

## Web UI Features
- **Pointer Lock Capture:** Click the trackpad to capture raw mouse movement (coalesced at 40 Hz), Left/Right/Middle clicks, scroll wheel (natural/reversed default + **Invert Scroll** toggle), and 6KRO keyboard input. Press `Esc` to unlock.
- **App Switcher (`Cmd + Tab (Hold)` / `Alt + Tab (Hold)`):** Presses `Cmd+Tab` (or `Alt+Tab`), releases `Tab` after 35 ms, and keeps `Cmd`/`Alt` held across your first click to enter Pointer Lock — releasing `Cmd`/`Alt` only after you register a click from **within** the captured mouse area (or click **Release All**). Click the button again to cycle apps.
- **Multi-Key & OS Shortcuts (`Cmd + Space`, etc.):**
  1. **CapsLock → Cmd** toggle (enabled by default): hold `CapsLock` + press `Space` to send `Cmd + Space` without triggering local Spotlight.
  2. **Sticky Modifiers bar:** Click `Ctrl` / `Shift` / `Alt` / `Cmd` to arm for the next key (double-click to lock).
  3. **Quick Combos bar:** One-click macros (`Cmd + Space`, `Cmd + Tab`, `Alt + Tab`, `Cmd + C/V/Z`, `Ctrl + Alt + Del`, `Esc`, `F4`, `F5`).
- **Connection Verification:** Top bar shows Web App WS dot, `Badge: online/offline`, `BLE: <peer>`, `Batt: %`, and live `RTT: ms`.

## Binary Wire Protocol (Client → Backend → Badge)
| Opcode | Name | Payload | Size |
|---|---|---|---|
| `0x01` | Mouse | `[0x01, buttons_u8, dx_i16_le, dy_i16_le, wheel_i8]` | 7 B |
| `0x02` | Keyboard 6KRO | `[0x02, modifiers_u8, k0..k5]` | 8 B |
| `0x03` | Macro Tap | `[0x03, modifiers_u8, k0..k2, hold_ms_u8]` | 6 B |
| `0x04` | Release All | `[0x04]` | 1 B |

## Troubleshooting
- **`ws err 1` on badge (`ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`):** The badge cannot reach the server's LAN IP. Verify both the badge and host are on the **same 2.4 GHz Wi-Fi network** (not Guest Wi-Fi / hotspot), check router AP/Client Isolation, and disable VPNs blocking LAN traffic.
- **`init: ESP_ERR_NO_MEM` on badge:** Regenerate `sdkconfig` from `sdkconfig.defaults` (`rm -f sdkconfig && idf.py build`) so PSRAM offload and trimmed Wi-Fi/BLE buffer settings are active.
