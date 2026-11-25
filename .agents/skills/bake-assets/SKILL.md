---
name: bake-assets
description: Regenerate app icons, planet orb textures, or documentation screenshots in The Badge without hand-editing generated C/H arrays.
---

# Bake Assets

Never edit `main/assets/app_icon_*.c`, `main/assets/icon_*.c`, `main/assets/orb_tex_*.h`, or `main/assets/assets.h` by hand.

## App Icons
1. Build the simulator: `python3 sim/build.py`
2. If live-rendered icons (water, moon, earth) changed, capture them first:
   `python3 tools/capture-icons.py` (writes `shots/icon_*.png` with UI buttons hidden via `#`).
3. Bake C arrays and sync `main/CMakeLists.txt`:
   `python3 tools/mkassets.py` (automatically runs `tools/sync_cmake.py`).

## Planet Orb Textures
1. Bake from an equirectangular photo:
   `python3 tools/make-orb-texture.py <moon|earth|sun|jupiter> <image-path>`
2. Because `main/apps/orb.c` uses `__has_include`, force a rebuild when adding a new texture header:
   `touch main/apps/orb.c`
3. Verify flash budget (each orb texture is ~256 KB; app partition is 6 MB with >= 512 KB headroom).

## Docs Screenshots
1. Build the simulator: `python3 sim/build.py`
2. Capture screenshots into `docs/img/`:
   `python3 tools/capture-shots.py`
3. Rebuild and run `tools/regress.sh` to verify nothing broke.
