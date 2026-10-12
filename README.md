# Ballet Photophonique

Firmware for Ballet Photophonique's PWM controller: five outputs, each with its own duty cycle, frequency and resolution, set from a web interface served by the device itself, over Art-Net, or from any [TesserAPI](https://github.com/leokeba/TesserAPI) client.

It runs on a [Gledopto GL-C-611WL](https://www.gledopto.eu/gledopto-gl-c-611wl-elite-5p-exmu-pwm-wled-controller) (ESP32, five low-side PWM outputs, 5–48 V, 15 A) and is an application of three libraries:

- [TesserKIT](https://github.com/leokeba/TesserKIT): Ethernet, Wi-Fi with a captive portal, time, OTA updates from this repository's releases, files and backups, timers, users and Art-Net, under `/sys`.
- [TesserAPI](https://github.com/leokeba/TesserAPI) and [TesserUI](https://github.com/leokeba/TesserUI): the device's tree, reachable over HTTP, WebSocket and USB serial, drawn in the browser without UI code.

This repository holds only the outputs: `/outputs/1` to `/outputs/5`, each with `on`, `duty` (%), `frequency` (Hz, fractions allowed) and `resolution` (bits), and `applied/`, what the timer really runs. Frequency and resolution share the timer's budget (`80 MHz / 2^bits` at most), so a combination that doesn't fit gets the nearest one rather than an error.

> **Status:** outputs, tree and host tests done; the firmware runs on the board (outputs not yet measured with a scope); Ethernet and GitHub releases built, not yet tried with a cable. The specification is [docs/DESIGN.md](docs/DESIGN.md).

## Building

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build    # host tests (ASan, UBSan)
. ~/esp/esp-idf/export.sh
idf.py -C firmware -B build/idf set-target esp32 build flash               # the firmware
```

TesserAPI, TesserUI and TesserKIT are expected next to this repository (`../TesserAPI`, ...), or through `TESSERAPI_PATH`, `TESSERUI_PATH` and `TESSERKIT_PATH`. A gitignored `firmware/main/secrets.h` may define `WIFI_SSID` and `WIFI_PASSWORD` for development; without it, a fresh controller opens an access point named `ballet-photophonique-xxxx` with a captive portal.

## Updates

Each push to `main` publishes a release (`.github/workflows/release.yml`). The controllers check for one daily and install it from `/sys/firmware/github`, or from the interface. A new board takes `ballet-photophonique-esp32-factory.bin` at offset 0 over USB:

```sh
uvx esptool --port <port> --baud 230400 write-flash 0 ballet-photophonique-esp32-factory.bin
```

The same workflow publishes the launcher at [leokeba.github.io/ballet-photophonique](https://leokeba.github.io/ballet-photophonique/): it reaches a controller over USB (in Chrome) or by its address.

## Controlling the outputs

```sh
curl -X POST http://<ip>/api/outputs/1 -d '{"on": true, "duty": 25, "frequency": 440.5, "resolution": 10}'
curl http://<ip>/api/outputs/1/applied        # {"frequency": 440.4995..., "bits": 10, "clock": "apb"}
```
