---
name: add-regression-guard
description: Record a fixed hardware or simulator bug as a semantic check in tools/regress.sh or a headless tools/sim-*.py script and document the root cause in CHANGELOG.md.
---

# Add Regression Guard

Use this workflow after fixing a bug that could silently return.

## Steps
1. **Choose the guard type:**
   - **Static invariant** (e.g., deletion order, RAM check before init, flag presence): add a semantic check to `tools/regress.sh`. Check the *meaning/invariant*, not brittle magic numbers or exact symbol names.
   - **Runtime UI/state bug** (e.g., swipe swallowed, stalled scan, timer paused while display off): add a focused headless script `tools/sim-<short-name>-check.py` driving `sim/badge_sim --serve`, and invoke it from `tools/regress.sh` if it runs in a few seconds.
2. **Document root cause:**
   - Add a brief entry to `CHANGELOG.md` explaining **what broke**, **why**, **the fix**, and **the check**.
3. **Verify:**
   - Rebuild the simulator if needed (`python3 sim/build.py`) and run `tools/regress.sh` plus the new `tools/sim-*.py` script.
