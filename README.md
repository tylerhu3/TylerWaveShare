# The Badge

Firmware for the Waveshare **ESP32-S3-Touch-AMOLED-1.75C** (466×466 round AMOLED, 32 MB flash,
8 MB PSRAM, no RTC). Forked from [curisama/The-Badge](https://github.com/curisama/The-Badge);
this repo is maintained independently.

<p align="center">
  <img src="docs/img/real/home.jpg" width="46%" alt="home screen">
  <img src="docs/img/real/water.gif" width="46%" alt="water fidget">
</p>

> **1.75C only.** The plain 1.75 has 16 MB flash and the partition table needs 32 MB.

## Apps

| | |
|---|---|
| **Trackpad / Air Mouse** | BLE HID mouse: touchpad, or point with the gyro |
| **Super Mouse** | USB host computer runs a shared web controller ([`super-mouse/`](super-mouse/)); the badge relays its mouse + keyboard to a BLE host |
| **Keys** | Ring of Excel keys (F1–F12, Esc, Tab, Ctrl+; …) over BLE HID |
| **Clock** | Clock, timer, stopwatch, alarm (rings with the app closed). Time comes from Wi-Fi NTP |
| **Meet** | Records to flash (~52 min, 16 kHz ADPCM); export as a read-only USB drive of `.WAV` files |
| **Games** | Bricks, Pinball, Marble, Pop |
| **Water / Orbit** | Accelerometer fidgets: sloshing water, Moon/Earth/Sun/Jupiter |
| **CheerLeader / Panda** | Pixel pet; looping panda GIF |
| **Calc / Settings** | Calculator; brightness, sound, timeout, time zone, Wi-Fi, BLE pairing |

## Flash

Browser flasher (Chrome/Edge): <https://curisama.github.io/The-Badge/flash/> — upstream's build, not this repo's.
To flash this repo, build it:

```sh
cp main/secrets.example.h main/secrets.h   # optional
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor  # /dev/ttyACM0 on Linux, COMx on Windows
```

Requires ESP-IDF 5.5+; see [docs/SETUP.md](docs/SETUP.md).

## Simulator

```sh
git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl.git sim/lvgl   # once
python3 sim/build.py
PORT=8791 python3 sim/server.py   # http://localhost:8791
tools/regress.sh                  # regression checks
```

## Notes

- `main/port.h` is the hardware seam: `port_esp.c` (board) / `sim/port_sim.c` (PC).
- Recording people may require consent depending on where you are.
- [CHANGELOG.md](CHANGELOG.md) records root causes of past bugs.

## License

MIT (`LICENSE`); third-party notices in `NOTICE`.
