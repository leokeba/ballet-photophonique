# Ballet Photophonique

Firmware for Ballet Photophonique's PWM controller: five outputs, each with its own duty cycle, frequency and resolution, set from a web interface served by the device itself, over Art-Net, or from any [TesserAPI](https://github.com/leokeba/TesserAPI) client.

It runs on a [Gledopto GL-C-611WL](https://www.gledopto.eu/gledopto-gl-c-611wl-elite-5p-exmu-pwm-wled-controller) (ESP32, five low-side PWM outputs, 5–48 V, 15 A) and is an application of three libraries:

- [TesserKIT](https://github.com/leokeba/TesserKIT): Ethernet, Wi-Fi with a captive portal, time, OTA updates from this repository's releases, files and backups, timers, users and Art-Net, under `/sys`.
- [TesserAPI](https://github.com/leokeba/TesserAPI) and [TesserUI](https://github.com/leokeba/TesserUI): the device's tree, reachable over HTTP, WebSocket and USB serial, drawn in the browser without UI code.

This repository holds only the outputs, MIDI and the recorder: `/outputs/1` to `/outputs/5`, each with `on`, `duty` (%), `frequency` (Hz, fractions allowed) and `resolution` (bits), and `applied/`, what the timer really runs. Frequency and resolution share the timer's budget (`80 MHz / 2^bits` at most), so a combination that doesn't fit gets the nearest one rather than an error.

> **Status:** outputs, tree and host tests done; the firmware runs on the board (outputs not yet measured with a scope); Ethernet and GitHub releases built, not yet tried with a cable. The specification is [docs/DESIGN.md](docs/DESIGN.md).

## Building

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build    # host tests (ASan, UBSan)
. ~/esp/esp-idf/export.sh
idf.py -C firmware -B build/idf set-target esp32 build flash               # the firmware
```

TesserAPI, TesserUI and TesserKIT are expected next to this repository (`../TesserAPI`, ...), or through `TESSERAPI_PATH`, `TESSERUI_PATH` and `TESSERKIT_PATH`. A gitignored `firmware/main/secrets.h` may define `WIFI_SSID` and `WIFI_PASSWORD` for development; without it, a fresh controller opens an access point named `ballet-photophonique-xxxx` with a captive portal.

## Playing from a MIDI keyboard

Plug the keyboard into the computer, and open the controller from the launcher, [leokeba.github.io/ballet-photophonique](https://leokeba.github.io/ballet-photophonique/), in Chrome or Edge (Web MIDI needs a secure page, which the controller's own `http://` page isn't). The first page, **Play**, has the keyboard: choose the MIDI input, then click a key (shift-click for a range, or play keys with **Learn**) and give it an output and, if you like, its own frequency. Velocity sets the duty; a new note on an output replaces the one it played; pitch bend moves the frequency or the duty (**Pitch bend**). The keyboard forwards MIDI while the Play page is open.

No MIDI keyboard? Play with the computer's keys, as in Ableton Live: A W S E D F T G Y H U J K O L P ; ' from C, Z and X for the octave, C and V for the velocity (the same physical keys on an AZERTY keyboard). This needs no secure page, so it also works from the controller's own `http://` page, in any browser. The keys turn on by themselves when no MIDI input is in use; **Computer keys** turns them on or off.

## Recording and replaying

The **Record** page has the keyboard, the recorder and the recordings. **Record** starts a new take (Take 1, Take 2...), which begins at its first note: play, then **Stop**, and it is saved on the controller as a Standard MIDI file. **Play** replays it from the controller alone, browser closed or not, through the key map in force; **Loop** starts it over at the end; **Overdub** records onto the current recording while it plays instead of starting a new take.

Edit in the piano roll: double-click for a note, drag to move it, drag its right edge to resize, Delete to remove, the arrows to nudge; drag on empty space to select several; the velocity field sets the selection's; **Quantize** snaps it to the grid; the lane under the notes is the pitch bend; the red handle is the end. Ctrl or Cmd+Z undoes. Recordings download as `.mid` for a DAW and upload back from the list under the recorder.

A show can start a recording without a browser: a timer (`/sys/timers`) calling `/recorder/play` or setting `/recorder/recording`, an Art-Net channel mapped to `/recorder/playing` (a gate) or `/recorder/cue` (the n-th recording), or any client:

```sh
curl -X PUT http://<ip>/api/recorder/recording -d '"Take 1"'
curl -X PUT http://<ip>/api/recorder/play -d null
```

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
