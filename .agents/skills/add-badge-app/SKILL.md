---
name: add-badge-app
description: Scaffold and register a new app or sub-screen in The Badge firmware while preserving the port.h HAL seam, CMake source list, and simulator compatibility.
---

# Add Badge App

Use this workflow when adding a new app (`main/apps/app_<name>.c`) or floating overlay screen.

## Steps
1. **Create the app file** in `main/apps/app_<name>.c`:
   - Include only `"app.h"`, `"port.h"`, `"lvgl.h"`, and standard C headers—never ESP-IDF headers.
   - Define `const badge_app_t app_<name>` with `name`, `art`/`icon`, `tint`, `radio` (`RADIO_OFF`, `RADIO_BLE`, or `RADIO_WIFI`), `keep_awake`, `enter(lv_obj_t *root)`, and `leave(void)`.
   - Never add `ui_back_btn()` on the app's first screen (use the launcher handle to exit).
   - If the app allocates PSRAM buffers (`port_big_alloc`), always delete the LVGL object (`lv_obj_delete`) **before** calling `port_big_free()` in `leave()`.
   - If using full-screen tap handling, ignore releases where pointer travel is `>= 60 px` so swipes do not trigger taps.
   - Never call `lv_timer_set_period()` inside a timer callback.
2. **Declare and register:**
   - Add `extern const badge_app_t app_<name>;` to `main/app.h`.
   - Register it in `main/launcher.c` (or the parent app menu).
   - If the file is firmware-only (not shared with simulator), add its filename to `ESP_ONLY` in `sim/build.py`.
3. **Sync CMake:**
   - Run `python3 tools/sync_cmake.py` (never hand-edit `SRCS` in `main/CMakeLists.txt`).
4. **Verify:**
   - Build simulator: `python3 sim/build.py`
   - Run `tools/regress.sh`
   - Run `python3 tools/sim-exit-check.py` (and `python3 tools/sim-stress.py 1` for interactive/animated screens).
