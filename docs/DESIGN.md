# Ballet Photophonique design

Status: phase 0 (2026-10-12): five outputs with duty, frequency and resolution, their tree and host tests; the firmware builds for the Gledopto GL-C-611WL with TesserKIT's services. This is the specification the code follows; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not built yet.

Ballet Photophonique's controller drives five PWM outputs, each with its own duty cycle, frequency and resolution, set from a web interface, over Art-Net, or from any TesserAPI client. It is an application of [TesserKIT](https://github.com/leokeba/TesserKIT) on [TesserAPI](https://github.com/leokeba/TesserAPI) and [TesserUI](https://github.com/leokeba/TesserUI), like [tiny-heliostat](https://github.com/leokeba/tiny-heliostat).

## 1. Goals

- **Five independent outputs.** Each output has its own timer, so its frequency and resolution never depend on another's. Two outputs at 440 and 440.5 Hz beat at 0.5 Hz.
- **Frequencies as asked, to a fraction of a hertz.** The timer's divider is computed here, not by ESP-IDF's driver, which takes whole hertz (§5.2). What the timer really runs is shown beside what was asked.
- **No combination refused.** A frequency and a resolution that don't fit together give the nearest the timer can do, so a console sweeping a frequency over Art-Net never sees a write refused.
- **Only the outputs live here.** Wi-Fi, time, updates, backups, timers, users and Art-Net are TesserKIT's (`/sys`); the tree and the interface are TesserAPI's and TesserUI's. A gap in a library is fixed there (§9), not worked around here.
- **Host-tested.** The timer arithmetic, the outputs and the tree run on the host with sanitizers, against a fake PWM (§10).

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
| Ethernet LAN8720 | RMII 19, 21, 22, 25, 26, 27; MDC 23, MDIO 33; PHY address 1; clock in on GPIO 0; oscillator enable GPIO 5 | WLED's "Gledopto Series With Ethernet" board (`wled00/network.cpp`). Not used until TesserKIT has Ethernet (§9). |
| PDM microphone | data 32, clock 15 | Not used. |

- GPIO 12 and 2 are strapping pins: the board holds them low at reset, and the outputs are attached only after boot.
- How fast the board's output stages switch is not documented: low-side MOSFETs on LED controllers are typically made for a few kHz to tens of kHz. Higher frequencies are produced by the chip but may not reach the terminals cleanly *(to measure with a scope)*.
- The pins are constants in `firmware/main/board.h`.

## 4. Architecture

```
 browser (TesserUI) ── HTTP / WebSocket ──▶ tesser::Api ◀── /sys: TesserKIT services (Art-Net writes /outputs/n/duty)
 USB serial ───────────────────────────────▶      │  /outputs (here)
                                                   ▼
                     Output setters (under the API lock) ── plan() ──▶ Pwm ──▶ LEDC timer and channel per output
```

### 4.1 Pieces of this repository

| Piece | What | Depends on |
|---|---|---|
| `timing` | `plan()`: the clock, divider and resolution for a frequency (§5.2); `dutyCounts()` | nothing |
| `Pwm` | the platform interface: attach, timing, duty per output | `timing` |
| `pwm_esp` | `Pwm` on the classic ESP32's LEDC (§5.3) | ESP-IDF's LEDC driver and low-level layer |
| `Output`, `Outputs` | an output's settings, applied to its timer; the `/outputs` tree | TesserAPI, `Pwm` |
| `firmware/` | the device: TesserKIT services, pins, mounts, `begin()` | everything |

Everything but `pwm_esp` is platform-free and host-tested.

### 4.2 Tasks and locks

There is no task of our own. The tree's setters run in whichever task handles the request (httpd, the UART reader, TesserKIT's task for Art-Net and Timers), under TesserAPI's lock, and write LEDC registers directly: a few register writes, no waiting. Nothing else touches an output.

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
- The low-level layer isn't a stable API. Built against ESP-IDF 6.1-dev (`esp_hal_ledc`); not yet against 5.5, TesserKIT's CI image, where `hal/ledc_ll.h` lives in the `hal` component: check it when moving ESP-IDF.

### 5.4 Art-Net

TesserKIT's `/sys/artnet` maps universe 0's channels 1–2 to output 1's duty (16 bits, 0–100 %), 3–4 to output 2's, and so on to 9–10 for output 5. **Off by default** (`artnet.enable(false)`), so a console on the network can't take the outputs over until someone turns Art-Net on. Mappings are editable in the interface; TesserKIT holds at most 8, so not every output's duty and frequency fit at once (§9).

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
```

## 7. Interface

TesserUI's generic renderer draws `/outputs` as a card per output: a toggle, a duty slider, frequency entry, a resolution slider and the applied values beside them. *(later)* A view with the five outputs side by side, once used.

## 8. Persistence and first flash

- TesserAPI persistence in NVS: one record (`outputs`), debounced 2 s. Stored backups in `Files` (LittleFS).
- **Partition table:** TesserKIT's `tesserkit_4mb_files.csv` (two 1.75 MB OTA slots, LittleFS for `Files`, a core dump). The first flash is over USB (bootloader too: rollback is a bootloader setting); updates are OTA after that. Gledopto's WLED is kept in `.local/` (gitignored) to restore with `esptool write-flash 0`.

## 9. Requests to other layers

| Request | Layer | For | Status |
|---|---|---|---|
| Ethernet: RMII with a LAN8720 (address, MDC, MDIO, clock mode and pin, power pin), beside Wi-Fi, under `/sys/network/ethernet` | TesserKIT | the GL-C-611WL's port, for a show network (§3) | *(later)* in TesserKIT DESIGN §5.2: to ask |
| More than 8 Art-Net mappings (a duty and a frequency per output is 10) | TesserKIT | §5.4 | to ask |
| A PWM output part with runtime frequency and resolution, if another device needs one | TesserMotion | `Pwm` and `Output` | not asked: TesserMotion's `Pwm` attaches at a fixed frequency; this repository's stays here until a second user |

## 10. Testing

1. **Host tests** (CMake, ASan, UBSan, `build/photophonique_tests`): `plan()` against hand-computed dividers (ESP-IDF's 5 kHz, 13-bit example), fractional frequencies, the resolution lowered and raised, a sweep of every frequency and resolution landing within one divider step; duty rounding; outputs on the fake PWM (stored until begun, rescaled duty, All off, a refused pin); the tree (ranges, patches in either order, persistence and restore).
2. **On the board** *(later)*: `test/e2e_outputs.py` over HTTP, and a scope on the terminals: frequency and duty against `applied/`, at the limits of each clock.

## 11. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 *(built)* | Repository, outputs, tree, host tests, firmware with TesserKIT's services | Host tests green; the firmware builds (1.08 MB, 38 % of the slot free) |
| 1 | On the board: first flash over USB, outputs measured with a scope, Art-Net from a console | Each output's frequency and duty within a divider step of `applied/` |
| 2 | Ethernet (TesserKIT), the Function button, releases from GitHub | The controller on a show network without Wi-Fi |

## 12. Open questions

- Is Art-Net the show's control, or something else (OSC, MQTT, TouchDesigner over WebSocket)? Which channels for which settings?
- Should outputs restore their last state after a power cut (now: yes, `on` is persisted), or always boot dark?
- The repository's home and license (public and MIT like its siblings?), and the GitHub repository for releases (`firmware.github()`).
- What frequencies the show needs: the board's output stages limit how high the terminals follow (§3).

## 13. Decisions

- **TesserKIT application, ESP-IDF, the GL-C-611WL** (2026-10-12): the controller is a TesserKIT device like tiny-heliostat, with `/sys` for everything but the outputs.
- **One timer per output** (2026-10-12): independent frequencies and resolutions; five of the ESP32's eight timers.
- **The divider computed here** (2026-10-12): fractional hertz, and the frequency actually run, known before it is applied.
- **Requests never refused as a pair; the nearest timing applied and shown** (2026-10-12).
