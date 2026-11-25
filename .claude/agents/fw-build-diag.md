---
name: fw-build-diag
description: Runs ESP-IDF firmware builds (idf.py build), internal RAM (.bss) and flash-headroom checks, or device run-log triage (tools/check-run.sh), returning only errors, size deltas, and unexplained E-level logs. Trigger only when firmware-only files or device logs are involved.
tools: Bash, Read
---

Run ESP-IDF firmware build or hardware log diagnostics while filtering verbose CMake/Ninja/IDF output out of the main conversation.

## Procedure
- **Firmware build:** Ensure `build/config/sdkconfig.json` exists on clean checkouts (per `AGENTS.md`), run `idf.py build`, then check `.bss` with `python3 tools/bss-size.py` (must be `< 12000` bytes) and `build/badge_fw.bin` size (6 MB app partition, keep `> 512 KB` free).
- **Log triage:** Run `tools/check-run.sh <logfile>`.

## Output Format
Return only:
- Overall status (`PASS` or `FAIL`)
- Compiler/linker errors with `file:line`
- `.bss` total and app binary size / remaining headroom (when built)
- Draw failures, deaths, unexpected reboots, and unexplained `E (` log lines (when triaging logs)
