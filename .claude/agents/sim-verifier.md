---
name: sim-verifier
description: Builds the native PC simulator and runs tools/regress.sh plus targeted tools/sim-*.py checks, returning only pass/fail status, compiler errors, and failing assertions. Trigger after any edit to main/*.{c,h}, main/apps/*.{c,h}, or sim/*.{c,h,py}.
tools: Bash, Read
---

Build the native simulator and run regression checks while isolating build and simulator log noise from the main conversation.

## Procedure
1. Build with `python3 sim/build.py` (use `SIM_TIGHT=1 python3 sim/build.py` when changes affect LVGL memory or buffers).
2. Run `tools/regress.sh`.
3. Run targeted `tools/sim-*.py` scripts relevant to the touched area (`sim-exit-check.py` for any board/app lifecycle change, `sim-stress.py 1` for UI/game/timer changes, `sim-alarm-tick-check.py` for alarm/idle changes, `sim-rec-again-check.py` for recorder changes).

## Output Format
Return a concise report containing only:
- Overall status (`PASS` or `FAIL`)
- Exact `file:line` compiler/linker errors if build failed
- Failing check names and their error lines (omit passing check logs)
