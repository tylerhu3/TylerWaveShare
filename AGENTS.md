# The Badge — Agent Context

ESP-IDF 5.5+ / LVGL 9.5.0 C firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
(466×466 round AMOLED, 32 MB flash, 8 MB octal PSRAM, no RTC), plus a native PC simulator.

## Architecture & Directory Map
- `main/` — Firmware core (`main.c`, `launcher.c`, `display.c`, `rec_store.c`, `usb_msc.c`, `super_mouse_usb.c`).
- `main/port.h` — Strict HAL seam. `main/port_esp.c` = hardware; `sim/port_sim.c` = simulator.
- `main/apps/` — One `.c` per app (`app_*.c`, including `app_super_mouse.c`) + overlays (`keypad.c`, `wifi_setup.c`, `host_pick.c`, `usb_screen.c`, `orb.c`).
- `main/ble/` — Firmware-only BLE HID peripheral (`hid_mouse.c`, `time_svc.c`, vendored `esp_hid_gap.*`).
- `main/assets/` — Generated LVGL C/H arrays (icons + planet textures). Do not hand-edit.
- `sim/` — Desktop simulator (`build.py`, `main_sim.c`, `port_sim.c`, `server.py`).
- `super-mouse/` — WebSerial web controller (+ tiny localhost static server) for `app_super_mouse.c`.
- `tools/` — Regression checks (`regress.sh`, `sim-*.py`), asset baking (`mkassets.py`), `sync_cmake.py`.
- `docs/` — GitHub Pages site, WebSerial flasher (`docs/flash/`), setup guide (`docs/SETUP.md`).

## Commands

### Simulator (fast local loop — no ESP-IDF required)
```sh
# One-time LVGL fetch for sim-only (do NOT clone into managed_components/):
git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl.git sim/lvgl

# Build simulator (cached liblvgl.a; add SIM_TIGHT=1 to mirror hardware RAM):
python3 sim/build.py

# Run full static + core simulator regression suite:
tools/regress.sh

# Run a single targeted simulator check:
python3 tools/sim-exit-check.py        # enter/exit all 6 boards
python3 tools/sim-stress.py 1          # 1 round of jitter + random input
python3 tools/sim-hold-check.py        # long-press across screen rebuild
python3 tools/sim-swipe-check.py       # home page swipe over icons
python3 tools/sim-wifi-stall-check.py  # stalled Wi-Fi scan recovery
python3 tools/sim-alarm-tick-check.py  # alarm ticks with display off
python3 tools/sim-rec-again-check.py   # rapid back-to-back recording

# Interactive browser UI (restart after rebuilding badge_sim):
PORT=8791 python3 sim/server.py
```

### Firmware & Build Maintenance
```sh
# Rebuild main/CMakeLists.txt after adding/removing any .c file under main/:
python3 tools/sync_cmake.py

# Clean-checkout ESP-IDF 5.5 pre-seed (prevents early LV_USE_* solver abort):
mkdir -p build/config && printf '{"LV_USE_FREETYPE":false,"LV_USE_LIBJPEG_TURBO":false,"LV_USE_LIBPNG":false,"LV_USE_LZ4":false}\n' > build/config/sdkconfig.json

# Build & flash firmware (ESP-IDF v6.0.2 on this machine):
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py -p /dev/cu.usbmodem* flash monitor   # /dev/ttyACM0 on Linux, COMx on Windows

# Super Mouse web UI (open http://localhost:8765 in Chrome/Edge):
cd super-mouse && npm install && npm run build && npm start

# Check permanent internal RAM (.bss must stay < 12 KB) & device logs (unverified locally):
python3 tools/bss-size.py
tools/check-run.sh <logfile>
```
*(No standalone linter/formatter is configured; `sim/build.py` enforces `-Werror=switch`, `-Werror=implicit-function-declaration`, `-Werror=return-type`, `-Werror=incompatible-pointer-types`, `-Werror=int-conversion` on project C files.)*

## Core Non-Obvious Conventions
- **Language & comments:** English-only for all internal code, comments, commit messages, and docs. Keep comments terse and high-signal (explain non-obvious hardware/LVGL invariants, not narration).
- **HAL seam:** Files in `main/apps/` and shared `main/` files must only call `port.h`, `app.h`, and LVGL—never ESP-IDF headers. Add new firmware-only `.c` files to `ESP_ONLY` in `sim/build.py`.
- **CMake sync:** Never hand-edit `SRCS` in `main/CMakeLists.txt`; run `python3 tools/sync_cmake.py`.
- **Memory lifecycle:** Always delete the LVGL canvas/object **before** freeing its backing buffer (`port_big_free`). Allocate large buffers dynamically in PSRAM (`port_big_alloc`), never as static arrays (`.bss` budget < 12 KB).
- **LVGL timers:** Never call `lv_timer_set_period()` inside a timer callback (causes 100% CPU loop). Skip ticks instead.
- **Radio mutual exclusion:** Wi-Fi and BLE share one 2.4 GHz antenna; no app runs both at once (coexistence adds 100–320 ms gaps, see `super-mouse/LATENCY-LOG.md`). Check `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` before `esp_hid_gap_init()`.
- **Super Mouse (USB source → BLE target):** the computer on the USB cable runs the web app, which writes sync-prefixed HID frames to the badge's USB-Serial/JTAG port via WebSerial; `super_mouse_usb.c` polls the USJ RX FIFO with the LL API (no driver install: it would make console writes block when nobody reads) and forwards to BLE HID only. Status goes back as `@SM {json}` console lines. See `super-mouse/AGENTS.md`.
- **Navigation:** First-level app screens exit via the launcher handle only—never add `ui_back_btn()` on an app's root screen. Full-screen tap targets must ignore drags >= 60 px so swipes do not trigger actions.
- **Conditional includes:** `orb.c` uses `__has_include` for textures; `touch main/apps/orb.c` after baking a new `orb_tex_*.h` so `sim/build.py` rebuilds it.

## Do Not Touch & Known Gotchas
- **Generated assets:** Do not edit `main/assets/app_icon_*.c`, `main/assets/icon_*.c`, `main/assets/orb_tex_*.h`, or `main/assets/assets.h` directly—regenerate via `tools/mkassets.py` or `tools/make-orb-texture.py`.
- **Vendored / external code:** Do not edit `main/ble/esp_hid_gap.{c,h}`, `managed_components/`, or `sim/lvgl/`.
- **Config & secrets:** Never commit `sdkconfig`, `main/secrets.h`, or `main/snippets.h`. Put persistent Kconfig changes in `sdkconfig.defaults`.
- **Partition offsets:** Changing `partitions.csv` requires updating flash offsets in `.github/workflows/build.yml` (`manifest.json`) and `docs/flash/`.
- **Large files:** Read `main/port_esp.c` (2.6k LOC) and `main/apps/app_games.c` (1.8k LOC) by targeted line ranges, not whole-file reads.

## Deeper Docs
- [README.md](README.md) — Hardware differences (1.75C vs 1.75), features, overview.
- [docs/SETUP.md](docs/SETUP.md) — Cross-platform toolchain & simulator setup.
- [CHANGELOG.md](CHANGELOG.md) — Root-cause history of past regressions.
- [docs/flash/README.md](docs/flash/README.md) — WebSerial flasher & manifest contract.
