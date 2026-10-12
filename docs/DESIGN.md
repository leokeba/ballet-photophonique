# Ballet Photophonique design

Status: phase 0 (2026-10-12): five outputs with duty, frequency and resolution, their tree and host tests; the firmware runs on the Gledopto GL-C-611WL with TesserKIT's services (flashed over USB, on Wi-Fi), outputs set over HTTP and serial but not yet turned on or measured. Ethernet and updates from this repository's GitHub releases since 2026-10-12 (§8). A MIDI keyboard on the computer plays the outputs through the launcher (§5.5, §7), tested end to end with a virtual keyboard. This is the specification the code follows; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not built yet.

Ballet Photophonique's controller drives five PWM outputs, each with its own duty cycle, frequency and resolution, set from a web interface, over Art-Net, from a MIDI keyboard plugged into the computer showing the interface, or from any TesserAPI client. It is an application of [TesserKIT](https://github.com/leokeba/TesserKIT) on [TesserAPI](https://github.com/leokeba/TesserAPI) and [TesserUI](https://github.com/leokeba/TesserUI), like [tiny-heliostat](https://github.com/leokeba/tiny-heliostat).

## 1. Goals

- **Five independent outputs.** Each output has its own timer, so its frequency and resolution never depend on another's. Two outputs at 440 and 440.5 Hz beat at 0.5 Hz.
- **Frequencies as asked, to a fraction of a hertz.** The timer's divider is computed here, not by ESP-IDF's driver, which takes whole hertz (§5.2). What the timer really runs is shown beside what was asked.
- **No combination refused.** A frequency and a resolution that don't fit together give the nearest the timer can do, so a console sweeping a frequency over Art-Net never sees a write refused.
- **Only the outputs live here.** Ethernet and Wi-Fi, time, updates, backups, timers, users and Art-Net are TesserKIT's (`/sys`); the tree and the interface are TesserAPI's and TesserUI's. A gap in a library is fixed there (§9), not worked around here.
- **Played from a keyboard.** Each key chooses its output and, optionally, its frequency; velocity is the duty; pitch bend moves the frequency or the duty (§5.5). Without a MIDI keyboard, or without a secure page, the computer's keys play, as in Ableton Live (§7).
- **Host-tested.** The timer arithmetic, the outputs, MIDI and the tree run on the host with sanitizers, against a fake PWM (§10).

### Non-goals

- Other boards than the GL-C-611WL (another classic ESP32 needs only its pins in `board.h`).
- Fades and envelopes on the device *(later, if a client can't send them fast enough)*.
- Phase between outputs: each output runs its own timer, started at boot, so their phases are arbitrary *(later: LEDC's `hpoint`, for outputs at the same frequency)*.
- The microphone.

## 2. Glossary

| Term | Meaning |
|---|---|
| Duty | The share of each period the output is high, 0–100 %. 100 % is always on. |
| Frequency | Periods per second, Hz. |
| Resolution | How many duty steps a period has, as a power of two: 8 bits is 256 steps. |
| Divider | The LEDC timer's clock divider, a fixed-point number with 8 fractional bits (1/256 steps), from 1 to 1023.996. |

## 3. Hardware

**Gledopto GL-C-611WL, "Elite 5P-EXMU PWM 5-in-1 WLED controller"** ([manual](https://www.gledopto.eu/mediafiles/anleitungen/7027-gl-c-611wl-eng.pdf)): 5–48 V input, 15 A in total, 10 A per output, a USB-C port (CH340-type UART, 5 V), Ethernet. The unit on the bench, read with esptool on 2026-10-12: **ESP32-D0WD-V3** (rev 3.1), 4 MB flash, no PSRAM, MAC 04:b2:47:84:92:68.

| Signal | GPIO | Notes |
|---|---|---|
| Outputs 1–5 | 18, 16, 12, 4, 2 | The terminals in the order printed on the case (`V+ IO18 IO16 IO12 IO4 IO2`). Low-side switches: high is on. |
| Function button | 17 | Not used yet *(later)*. |
| Spare terminal (IO13) | 13 | Not used. |
| Ethernet LAN8720 | RMII 19, 21, 22, 25, 26, 27; MDC 23, MDIO 33; PHY address 1; clock in on GPIO 0; oscillator enable GPIO 5 | WLED's "Gledopto Series With Ethernet" board (`wled00/network.cpp`), TesserKIT's `tesserkit::boards::gledopto()`. |
| PDM microphone | data 32, clock 15 | Not used. |

- GPIO 12 and 2 are strapping pins: the board holds them low at reset, and the outputs are attached only after boot.
- How fast the board's output stages switch is not documented: low-side MOSFETs on LED controllers are typically made for a few kHz to tens of kHz. Higher frequencies are produced by the chip but may not reach the terminals cleanly *(to measure with a scope)*.
- The pins are constants in `firmware/main/board.h`; the Ethernet wiring is TesserKIT's preset.
- **Ethernet** (`net.ethernet(tesserkit::boards::gledopto())`, `/sys/network/ethernet`): preferred to Wi-Fi while it has an address, the station joining only without one (TesserKIT DESIGN §5.2). A controller on a cable with no Wi-Fi settings opens its access point only after 15 s without an address.

## 4. Architecture

```
 browser (TesserUI, x-midi ◀─ Web MIDI) ── HTTP / WebSocket ──▶ tesser::Api ◀── /sys: TesserKIT services (Art-Net writes /outputs/n/duty)
 USB serial ───────────────────────────────▶      │  /outputs (here)
                                                   ▼
   /midi/send ─▶ Midi ─▶ Output setters (under the API lock) ── plan() ──▶ Pwm ──▶ LEDC timer and channel per output
```

### 4.1 Pieces of this repository

| Piece | What | Depends on |
|---|---|---|
| `timing` | `plan()`: the clock, divider and resolution for a frequency (§5.2); `dutyCounts()` | nothing |
| `Pwm` | the platform interface: attach, timing, duty per output | `timing` |
| `pwm_esp` | `Pwm` on the classic ESP32's LEDC (§5.3) | ESP-IDF's LEDC driver and low-level layer |
| `Output`, `Outputs` | an output's settings, applied to its timer; the `/outputs` tree | TesserAPI, `Pwm` |
| `Midi` | MIDI messages onto the outputs: the key map, voices, pitch bend; the `/midi` tree (§5.5) | `Outputs` |
| `x-midi` (`web/elements/midi.js`) | the custom element: Web MIDI in the browser, the keyboard and its map (§7) | TesserUI's custom element contract |
| `firmware/` | the device: TesserKIT services, pins, mounts, `begin()` | everything |

Everything but `pwm_esp` and the element is platform-free and host-tested; the element is tested in Chromium against the board (§10).

### 4.2 Tasks and locks

There is no task of our own. The tree's setters run in whichever task handles the request (httpd, the UART reader, TesserKIT's task for Art-Net and Timers), under TesserAPI's lock, and write LEDC registers directly: a few register writes, never waiting (§5.3). A duty that must wait for its channel's period is applied by `Pwm::poll()`, called from TesserKIT's tick (`dev.onTick`, every 20 ms) under the same lock. Nothing else touches an output.

## 5. Behaviour

### 5.1 Outputs

- Each output has a persisted `on`, `duty`, `frequency` and `resolution`. Factory defaults: off, 50 %, 1000 Hz, 12 bits.
- **Off** holds the output low (duty 0, the timer still running), so turning it on starts at the next period.
- **Boot:** the pins are attached after `dev.begin()` has loaded the stored settings, so an output goes from low (the board's pull-down) straight to its stored state. A fresh controller, or one after a factory reset, has every output off.
- **All off** (`/outputs/off`) turns every output off, announced to subscribers.

### 5.2 Frequency and resolution

The LEDC timer counts `2^bits` steps per period from a clock divided by the divider:

```
frequency = clock · 256 / (divider · 2^bits)      divider: 256 … 262143 (1.0 … 1023.996)
```

- **Clocks:** APB (80 MHz) and the reference tick (1 MHz). Both can be chosen per timer in both speed groups. RC_FAST (8 MHz) isn't used: on the classic ESP32 it is a global choice for the low-speed group.
- **`plan(hz, bits)`:** the divider is rounded to the nearest step. APB is tried first, the tick second; the first whose divider is in range wins (APB's divider is 80 times larger, so its steps are finer). When neither fits:
  - too fast (APB's divider would be under 1): the highest resolution that reaches the frequency;
  - too slow (the tick's divider would be over 1023.996): the lowest resolution that does.
- **What is reachable:** 0.001 Hz (20 bits, tick) to 40 MHz (1 bit, APB), so `frequency` has that range, and every value in it lands within half a divider step: at most 1/512 off, usually much less. At 8 bits near 440 Hz a step is 0.002 Hz; at 16 bits, 0.6 Hz. Fine frequency costs resolution.
- **Resolution and frequency** at most: `80 MHz / 2^bits`: 312.5 kHz at 8 bits, 19.5 kHz at 12, 1.2 kHz at 16. At least: `1 MHz / (1024 · 2^bits)`: 3.8 Hz at 8 bits, 0.24 Hz at 12.
- **`applied/`** shows what the timer runs: the actual frequency, the bits and the clock. `resolution` keeps what was asked, so lowering the frequency again restores it.
- **Duty** is rounded to the nearest of the `2^bits + 1` steps (0 to always on), and re-applied whenever the resolution changes, so 50 % stays 50 %.
- Frequency and resolution are never refused as a pair. TesserAPI validates each value of a patch against its siblings' old values (TesserAPI §6.3), so a cross-check would refuse `{"frequency": 1000000, "resolution": 6}` sent from 12 bits. And a mapped Art-Net channel sweeping the frequency would stop at the first refusal.

### 5.3 On the ESP32

- Output `i` uses LEDC timer and channel `i mod 4` of the high-speed group for `i < 4`, of the low-speed group after: five independent timers, three left.
- `ledc_timer_config()` takes whole hertz and computes its own divider, so it only sets a timer up (APB, 10 bits, 1 kHz, which also makes APB the low-speed group's slow clock). `plan()`'s divider, resolution and clock are then written with ESP-IDF's low-level layer (`ledc_ll_set_clock_divider`, `ledc_ll_set_duty_resolution`, `ledc_ll_set_clock_source`, and `ledc_ll_ls_timer_update` for the low-speed group). The driver reads the resolution back from the register for its duty checks, so `ledc_set_duty()` stays usable.
- **A duty takes effect at the end of the period.** On the classic ESP32, `ledc_update_duty()` then spins, in a critical section, until the previous duty was applied: at 0.5 Hz that outlasts the interrupt watchdog, and the board panicked (2026-10-12, at boot too, from the stored state). So `duty()` writes only when the channel's `duty_start` bit is clear, and otherwise keeps the latest value for `poll()`. At low frequencies a duty change thus shows within two periods; intermediate values sent within one period are dropped, the latest wins.
- **A resolution change restarts the period** (`ledc_timer_rst`): a counter already past the new top would otherwise run on to the 20-bit wrap, minutes on the 1 MHz tick. Frequency changes keep the period running.
- The low-level layer isn't a stable API. Built against ESP-IDF 6.1-dev (`esp_hal_ledc`) and 5.5 (the release workflow's image, where `hal/ledc_ll.h` lives in the `hal` component). `LEDC_LL_GET_HW()` takes a group from 6.0 on, so the registers are reached as `&LEDC` (`soc/ledc_struct.h`, the same in both). Check it when moving ESP-IDF.

### 5.4 Art-Net

TesserKIT's `/sys/artnet` maps universe 0's channels 1–2 to output 1's duty (16 bits, 0–100 %), 3–4 to output 2's, and so on to 9–10 for output 5. **Off by default** (`artnet.enable(false)`), so a console on the network can't take the outputs over until someone turns Art-Net on. Mappings are editable in the interface; TesserKIT holds at most 8, so not every output's duty and frequency fit at once (§9).

### 5.5 MIDI

A MIDI keyboard plugged into the computer showing the interface plays the outputs: the browser reads it (Web MIDI, in `x-midi`, §7) and forwards its messages to `/midi/send`; the controller does the rest, so any client can play it the same way (TouchDesigner, a USB-MIDI host board later).

- **Key map** *(p)*: for each of the 128 notes, an output (0 for none, the default) and a frequency (0 for the note's own: equal temperament, A4 = 440 Hz). A fresh controller plays nothing until keys are mapped.
- **Note on:** the key's output takes the key's frequency and a duty of `velocity / 127 · 100 %`, and turns on. The output's previous note, if any, is dropped: an output plays one note, the latest. Unmapped keys do nothing.
- **Note off** (or a note on with velocity 0) turns off the output the note sounds on, if it still has it; a key remapped while held is still released.
- **Pitch bend** (14 bits, centre 8192, -1 to +1) applies to every sounding output and to the next notes: `target` `frequency` multiplies the frequency by `2^(bend · semitones / 12)` (default ±2 semitones); `duty` adds `bend · duty` percentage points (default ±50), clamped to 0–100 %.
- **All notes off** and **all sound off** (CC 123 and 120), and `/midi/off`, turn off every output a note holds.
- **Channels** are ignored (omni). Other messages are dropped.
- **Messages** come packed as `status | data1 << 8 | data2 << 16`, alone or in an array (a batch, applied in order; refused whole if one isn't a message).
- MIDI writes the outputs' settings like any client: they show in the interface, and are saved 2 s after the last change, like any setting (NVS wears evenly; a show's playing is far from its limits).

## 6. The tree

Persisted nodes are marked *(p)*. Reads are public; writes to `/outputs` need `User`; TesserKIT's Auth leaves everyone Admin until an admin account exists.

```
/outputs                label "Outputs", ui.icon bulb
  off                   action "All off", ui.style danger
  1 … 5/                label "Output n"
    on *(p)             boolean (default false)
    duty *(p)           number 0–100 %, step 0.1 (default 50)
    frequency *(p)      number 0.001–40 000 000 Hz (default 1000), ui.widget number
    resolution *(p)     integer 1–20 bits (default 12)
    applied/            ui.inline: frequency (Hz, actual), bits, clock (apb, refTick; advanced), read-only, watched
    gpio                integer, read-only, advanced

/midi                   label "MIDI", ui.icon play
  keys/                 ui.widget x-midi, ui.script /ext/midi.js
    outputs *(p)        array of 128 integers 0–5: each note's output, 0 none
    frequencies *(p)    array of 128 numbers 0–40 000 000 Hz: each note's frequency, 0 the note's own
    voices              array of 8 integers, read-only: the note each output plays, -1 none (announced)
  bend/                 label "Pitch bend"
    target *(p)         enum frequency, duty (default frequency)
    semitones *(p)      0–48 st (default 2), visibleWhen target=frequency
    duty *(p)           0–100 % (default 50), visibleWhen target=duty
    value               -1 to 1, read-only (announced)
  send                  action: a packed message or an array of them, ui.hidden
  off                   action "All notes off"
```

## 7. Interface

TesserUI's generic renderer draws `/outputs` as a card per output: a toggle, a duty slider, frequency entry, a resolution slider and the applied values beside them, and `/midi` with its keyboard and the bend settings.

**Play** (a view, the first page): the keyboard, a row per output (on, the applied frequency, the duty), All off and All notes off, and the pitch bend settings. The keyboard forwards MIDI only while it is on screen, so this page is where the show is played.

**`x-midi`** (`web/elements/midi.js`, plain JavaScript), over `/midi/keys`:
- **Web MIDI:** a choice of the computer's MIDI inputs (the last one chosen is remembered in the browser, else the first); notes, pitch bend and CC 120/123 forwarded to `/midi/send`, with one request in flight and what comes meanwhile sent as one batch, a newer bend replacing an unsent one. Web MIDI needs Chrome or Edge and a secure page: the launcher on GitHub Pages, not the controller's own `http://` page, where the element says so and still edits the map. Chrome asks once for MIDI access.
- **The computer's keys** (as Ableton Live's computer MIDI keyboard): A W S E D F T G Y H U J K O L P ; ' play an octave and a half from C (C4 at first), Z and X move it an octave, C and V step the velocity through 1, 20, 40, 60, 80, 100 (the first), 120, 127. Notes go through the same queue as Web MIDI's, so they need no secure page: the controller's own `http://` page plays too.
  - **On** by default while no MIDI input is in use (no Web MIDI, no keyboard plugged in, or None chosen); the **Computer keys** button turns it on or off, and that choice is remembered in the browser.
  - **Physical positions** (`KeyboardEvent.code`): an AZERTY keyboard plays the same places. The keys on screen are labelled with the layout's letters where Chrome gives them (its keyboard layout map, on secure pages such as the launcher), QWERTY's elsewhere. The bar shows the range, the velocity and the keys that move them.
  - **Not while typing:** keys go to fields (here or anywhere on the page) and with Ctrl, Alt or Cmd, as usual. A key's release turns off the note its press played, even after an octave change; losing the window's focus, turning the keys off or leaving the page releases the notes held.
- **The keyboard:** 88 keys (A0–C8), scrolled to middle C, each coloured and numbered by its output; keys held on the computer's keyboard darken, keys sounding on the controller (`voices`) have a dot.
- **Editing:** click a key, shift-click for a range, or play keys with **Learn** on; then an output for all of them, and a frequency for a single key (empty or **Note's own**: the note's). Each edit writes the whole `outputs` array, or one element of `frequencies`.
- **Served** from flash at `/ext/midi.js` (7 KB gzipped; `web/build.mjs` gzips it into `src/photophonique/elements.h`, committed), and on GitHub Pages next to the launcher, which loads it through `window.TESSER.scripts`: an HTTPS page can't load it from the controller's `http://` address. The element defines itself once, whichever copy comes first.
- **Latency:** from the launcher by address, each batch is one HTTP request: a few milliseconds over Ethernet, more and less regular over Wi-Fi.

## 8. Persistence, first flash and updates

- TesserAPI persistence in NVS: one record (`outputs`), debounced 2 s. Stored backups in `Files` (LittleFS).
- **Partition table:** TesserKIT's `tesserkit_4mb_files.csv` (two 1.75 MB OTA slots, LittleFS for `Files`, a core dump). The first flash is over USB (bootloader too: rollback is a bootloader setting), with `ballet-photophonique-esp32-factory.bin` at 0x0 or `idf.py flash`; updates are OTA after that. Gledopto's WLED is kept in `.local/` (gitignored) to restore with `esptool write-flash 0`.
- **Releases:** each push to `main` runs the host tests, then TesserKIT's reusable `firmware.yml` builds the firmware against the libraries' `main` branches (its ESP-IDF 5.5 image) and publishes `vX.Y.Z` (the last tag's patch raised) with `ballet-photophonique-esp32.bin` and `ballet-photophonique-esp32-factory.bin`. `[skip ci]` in a commit message releases nothing.
- **Updates:** `firmware.github("leokeba/ballet-photophonique", "ballet-photophonique-{target}.bin")`: the controller checks for a newer release a minute after coming online, then daily and on demand, and installs it from `/sys/firmware/github` (or the interface); a new image confirms itself after a minute online, else it rolls back (TesserKIT DESIGN §5.4). Uploads (`/sys/firmware/image`) and URLs work too.
- **Launcher:** the same workflow publishes TesserUI's launcher, built from the TesserUI the firmware has, to GitHub Pages: https://leokeba.github.io/ballet-photophonique/. It reaches a controller over USB (the API on UART 0) or by its address (`dev.cors("https://leokeba.github.io")`), with no network in common needed for USB.

## 9. Requests to other layers

| Request | Layer | For | Status |
|---|---|---|---|
| Ethernet: RMII with a LAN8720 (address, MDC, MDIO, clock mode and pin, power pin), beside Wi-Fi, under `/sys/network/ethernet` | TesserKIT | the GL-C-611WL's port, for a show network (§3) | done, TesserKIT 627088b (`boards::gledopto()`); works on the board over a cable (§10) |
| Turning `/sys/network/ethernet/static` off keeps the static address until a restart (seen 2026-10-12): the station, which waits while Ethernet has an address, then never joins. TesserKIT §5.2 says `static` is applied at once | TesserKIT | moving a controller from a cable to Wi-Fi | to report |
| ArtPollReply's MAC is the base MAC (…:bc) when the node is reached over Ethernet (…:bf), whose address the reply gives | TesserKIT | consoles listing nodes by MAC | minor; to mention |
| More than 8 Art-Net mappings (a duty and a frequency per output is 10) | TesserKIT | §5.4 | to ask |
| A PWM output part with runtime frequency and resolution, if another device needs one | TesserMotion | `Pwm` and `Output` | not asked: TesserMotion's `Pwm` attaches at a fixed frequency; this repository's stays here until a second user |

## 10. Testing

1. **Host tests** (CMake, ASan, UBSan, `build/photophonique_tests`): `plan()` against hand-computed dividers (ESP-IDF's 5 kHz, 13-bit example), fractional frequencies, the resolution lowered and raised, a sweep of every frequency and resolution landing within one divider step; duty rounding; outputs on the fake PWM (stored until begun, rescaled duty, All off, a refused pin); the tree (ranges, patches in either order, persistence and restore).
2. **MIDI in the browser** (`test/midi/`): `keyboard.py` opens a virtual CoreMIDI source (python-rtmidi), `serve.py` serves the launcher page, TesserUI's bundle and `midi.js` on localhost (a secure context) with `/api` proxied to a controller, and `e2e_midi.mjs` drives Chromium (Playwright, MIDI permission granted): the element finds the keyboard, notes reach their outputs with their frequency and velocity's duty, a bend moves two sounding outputs, a burst of three notes arrives batched, the keyboard shows the sounding keys, note off and all notes off, then the map edited by clicks, a shift-click range and a frequency typed and reset. 13 checks (the computer's keys off while a MIDI input plays, too), passing on 2026-10-12 against the second board over Ethernet, from localhost and from the published launcher itself (`https://leokeba.github.io/ballet-photophonique/?api=http://192.168.3.50/api`, v0.1.1: HTTP polling, CORS, the element from Pages). There the console reports the controller's own copy of the script blocked (CORS doesn't cover `/ext/`): expected, the page's copy defines the element.
3. **The computer's keys** (`test/midi/e2e_typing.mjs`), on the controller's own `http://` page (no secure context, the element from flash, live over WebSocket): the element says there is no Web MIDI and turns the keys on, C4 is labelled A, A plays C4 at velocity 100, C lowers the velocity, X raises the octave, a key released after Z still releases its note, typing in the frequency field and Ctrl+A play nothing, losing focus releases a held key, and the button turns the keys and their labels off. 17 checks, passing on 2026-10-12.
4. **On the board:** v0.1.0, the first release (ESP-IDF 5.5), found and installed from `/sys/firmware/github` over Wi-Fi on 2026-10-12, settings kept, confirmed after a minute online. Ethernet (2026-10-12, a second GL-C-611WL, MAC 20:50:0d:1b:02:bc, flashed with v0.1.0's factory image, cabled straight to a Mac's USB adapter, no DHCP on the link): link up at 100 Mbit/s, a static address set over USB serial (`/sys/network/ethernet/static`), then over the cable: ping (1.3 ms), the API and the interface, outputs set, ArtPoll answered with the Ethernet address and ArtDmx writing a duty, a 1.35 MB upload in 6.5 s (208 KB/s) booted and confirmed online on the cable alone, and a restart back on the cable in 3 s without its access point coming up (no Wi-Fi saved). Before the address was set, the access point came up after 15 s and went down 30 s after the address, as TesserKIT specifies. Not tried: unplugging the cable to fall back to Wi-Fi, DHCP over the cable. By hand on 2026-10-12, outputs off: writes over HTTP and serial across both clocks, both speed groups and periods up to 1000 s, with no reset; a stored 0.5 Hz state boots. *(later)* `test/e2e_outputs.py`, and a scope on the terminals: frequency and duty against `applied/`, at the limits of each clock, and a duty change at low frequency arriving within two periods.

## 11. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 *(built)* | Repository, outputs, tree, host tests, firmware with TesserKIT's services | Host tests green; the firmware builds (1.09 MB, 38 % of the slot free), boots on the board, and takes writes at every frequency and resolution without a reset |
| 1 | Outputs measured with a scope, Art-Net from a console | Each output's frequency and duty within a divider step of `applied/` |
| 2 *(built)* | Ethernet (TesserKIT), releases from GitHub, the launcher on Pages | The controller on a show network without Wi-Fi, updated from a release. Built: 1.27 MB on ESP-IDF 6.1-dev (27 % of the slot free), 1.29 MB on 5.5 |
| 3 | The Function button | |
| 4 *(built)* | MIDI: the key map, voices and bend on the controller; `x-midi` and the Play view; the computer's keys | Played from a keyboard through the Pages launcher. Released in v0.1.1; tested with a virtual keyboard from the Pages launcher (§10); not yet with a real keyboard |

## 12. Open questions

- Is Art-Net the show's control, or something else (OSC, MQTT, TouchDesigner over WebSocket)? Which channels for which settings?
- Should outputs restore their last state after a power cut (now: yes, `on` is persisted), or always boot dark?
- The license (MIT like its siblings?). The repository is public at https://github.com/leokeba/ballet-photophonique.
- What frequencies the show needs: the board's output stages limit how high the terminals follow (§3).

## 13. Decisions

- **TesserKIT application, ESP-IDF, the GL-C-611WL** (2026-10-12): the controller is a TesserKIT device like tiny-heliostat, with `/sys` for everything but the outputs.
- **One timer per output** (2026-10-12): independent frequencies and resolutions; five of the ESP32's eight timers.
- **The divider computed here** (2026-10-12): fractional hertz, and the frequency actually run, known before it is applied.
- **Requests never refused as a pair; the nearest timing applied and shown** (2026-10-12).
- **Ethernet, GitHub releases and the launcher on Pages** (Leo, 2026-10-12), as tiny-heliostat and TesserKIT do.
- **MIDI from the computer, through the launcher on Pages** (Leo, 2026-10-12): the classic ESP32 has no USB host. Each key chooses its output and optionally its frequency; velocity drives duty; a new note on an output drops the previous one; pitch bend assignable to frequency or duty.
- **The computer's keys, as in Ableton Live** (Leo, 2026-10-12): playable without a MIDI keyboard, and without a secure page. Physical key positions, so layouts other than QWERTY play the same places.
- **MIDI logic on the controller, the element only forwards** (2026-10-12): the map is stored with the other settings and backed up, any client plays the same way, and the logic is host-tested.
