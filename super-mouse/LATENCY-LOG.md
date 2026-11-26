# Super Mouse smoothness — what was tried and what failed

Goal: make Super Mouse (browser → server → badge → HID) feel as smooth as Air
Mouse (`main/apps/app_mouse.c`, IMU → BLE HID directly). Nothing below shipped;
the code is all reverted to `d503a28`. Read this before trying again.

## The root cause (measured)
On the badge, Wi-Fi and BLE share one ESP32-S3 radio. With Wi-Fi CONNECTED and
BLE CONNECTED, ESP-IDF coexistence splits each ~100 ms, beacon-aligned period
roughly 50/50 between them. Wi-Fi packets that arrive during the BLE half wait
at the access point. Badge logs showed gaps of 100–320 ms between frames
(`rx 42/s max_gap 268 ms`), clustered at multiples of ~100 ms. Air Mouse never
turns on Wi-Fi, which is why it has none of these gaps.

## Wi-Fi path tweaks (all failed: gaps unchanged)
Earlier attempts (Gemini), from its notes:
- BLE connection interval 7.5 ms.
- UDP fast path instead of WebSocket/TCP: worse.
- `WIFI_PS_MIN_MODEM` (it is required while BLE is on; `WIFI_PS_NONE` is not
  allowed with coexistence).
- Router beacon 102.4 ms / DTIM 3; `listen_interval=1`: no help.
- Server pacer drain modes; frontend send-rate tweaks: reverted.

Attempted here:
- Keep-awake traffic: browser sends a `0x05` "awake" frame, plus UDP keep-alive,
  to hold the radio on Wi-Fi. `max_gap` stayed 100–320 ms. It can't work:
  coexistence time-slices the radio no matter how much traffic there is.

## Bluetooth "bridge" source (built, then abandoned)
Idea: turn badge Wi-Fi off entirely. The server runs on the same Mac the badge
is paired with as a BLE mouse, and a small macOS helper (Swift,
CoreBluetooth) writes the frames to a custom GATT characteristic (`0000ba73-…`)
in the badge's `ba5e` service over that same BLE link:
browser → tunnel → server → helper → BLE → badge → HID.

What was built:
- Firmware: ba73 write characteristic in `time_svc.c` (8 handles in service
  `ba5e`; HID handles unchanged). Batched frame dispatch, a `0x05` heartbeat,
  a Bridge/Wi-Fi source switch saved in NVS, and a new Target/Source scrolling UI.
- Server: `ble-link.ts`, which spawns the helper and makes it look like a
  device WebSocket.
- Helper: `bridge/BadgeBridge.swift`, which merges queued mouse deltas and
  packs frames into each BLE write.

What went wrong:
1. The new UI used more internal RAM, and `esp_wifi_init` failed with
   `ESP_ERR_NO_MEM` ("Expected to init 4 rx buffer, actual is 0"). It was
   worked around with `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=2`, untested on
   hardware. `.bss` is already over the 12 KB budget at `d503a28` (12353 B).
2. The helper only works while macOS has the badge connected. Often the badge
   was connected to another host (the iPhone) or not at all, and the badge app
   sat at "Waiting for bridge". The badge doesn't advertise while it is
   connected, so a scan can't find it. `retrieveConnectedPeripherals` only
   returns it while this Mac holds the link (it then showed up under services
   `ba5e`, `180A` and `180F`, but not `1812`).
3. With both sources up, the server let the last connection to arrive own the
   room. The bridge took over from the badge's Wi-Fi socket, and frames went
   to a badge that was ignoring Bluetooth. Fixed in the server, but it was
   confusing.
4. UX: the Mac was the Target (HID host) and the Source (bridge) at the same
   time, over one link named "Badge Mouse". Users found this confusing. Two
   separate BLE identities ("Badge Mouse" + "Badge Mouse Input") were
   considered and rejected:
   - two links would jitter each other's timing
   - not enough internal RAM
   - it needs BLE 5 multi-advertising, which touches the vendored `esp_hid_gap`
5. "The device constantly tries to connect to the source computer" was
   reported and never diagnosed.
6. End-to-end smooth cursor movement over the bridge was never confirmed on
   hardware.

The full code is kept in `git stash` on branch
`AddBleToTargetAndSourceMachine` as "super-mouse BLE bridge attempt (reverted
2026-11-25)". Restore it with `git stash apply`.

## If trying again
- Don't fight coexistence on the Wi-Fi path with pacing, UDP or keep-alive
  traffic. The floor is ~100 ms gaps while both radios are connected.
- Options not yet tried:
  - A badge-side smoothing timer (replay received deltas at a steady 7.5–10 ms
    rate, like Air Mouse) to hide the gaps. This adds latency but removes
    the bursts.
  - USB HID Target with Wi-Fi Source. `port_sm_output_set(SM_OUT_USB)`
    already pauses BLE advertising and prefers Wi-Fi for coexistence, but
    `max_gap` was never measured in that mode. If a BLE host is still
    connected, BLE keeps its time slots.

## 2026-11-25: USB source replaces Wi-Fi
Super Mouse now takes its input from the computer on the USB cable: the web
page writes frames to the badge's USB-Serial/JTAG port with WebSerial, and the
badge forwards them to its BLE host (the only target; USB HID output was
removed). Wi-Fi is never started, so coexistence time-slicing is gone. Not yet
measured on hardware; if the cursor still stutters, compare the badge's
`rx` rate with the browser's send rate before blaming the BLE link.
