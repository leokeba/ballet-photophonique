# Ballet Photophonique design

Status: phase 0 (2026-10-12): five outputs with duty, frequency and resolution, their tree and host tests; the firmware runs on the Gledopto GL-C-611WL with TesserKIT's services (flashed over USB, on Wi-Fi), outputs set over HTTP and serial but not yet turned on or measured. Ethernet and updates from this repository's GitHub releases since 2026-10-12 (§8). A MIDI keyboard on the computer plays the outputs through the launcher (§5.5, §7), tested end to end with a virtual keyboard. The recorder (§5.6) since 2026-10-13: host-tested, and on the second board over USB (serial API and the piano roll in Chromium), not yet released. The API over UDP (`/sys/udp`) since 2026-10-13, on the way to the ensemble (§5.7), whose generic parts the libraries are building (§9). This is the specification the code follows; when the two disagree, fix one of them. Sections marked *(later)* are committed to but not built yet.

Ballet Photophonique's controller drives five PWM outputs, each with its own duty cycle, frequency and resolution, set from a web interface, over Art-Net, from a MIDI keyboard plugged into the computer showing the interface, or from any TesserAPI client. It is an application of [TesserKIT](https://github.com/leokeba/TesserKIT) on [TesserAPI](https://github.com/leokeba/TesserAPI) and [TesserUI](https://github.com/leokeba/TesserUI), like [tiny-heliostat](https://github.com/leokeba/tiny-heliostat).

## 1. Goals

- **Five independent outputs.** Each output has its own timer, so its frequency and resolution never depend on another's. Two outputs at 440 and 440.5 Hz beat at 0.5 Hz.
- **Frequencies as asked, to a fraction of a hertz.** The timer's divider is computed here, not by ESP-IDF's driver, which takes whole hertz (§5.2). What the timer really runs is shown beside what was asked.
- **No combination refused.** A frequency and a resolution that don't fit together give the nearest the timer can do, so a console sweeping a frequency over Art-Net never sees a write refused.
- **Only the outputs live here.** Ethernet and Wi-Fi, time, updates, backups, timers, users, Art-Net and the API over UDP (the other controllers on the LAN, under `/peers`) are TesserKIT's (`/sys`); the tree and the interface are TesserAPI's and TesserUI's. A gap in a library is fixed there (§9), not worked around here.
- **Played from a keyboard.** Each key chooses its output and, optionally, its frequency; velocity is the duty; pitch bend moves the frequency or the duty (§5.5). Without a MIDI keyboard, or without a secure page, the computer's keys play, as in Ableton Live (§7).
- **Recorded, replayed and edited.** What is played is recorded on the controller as Standard MIDI files, replayed by the controller alone (looped, overdubbed, started by any client: a timer, Art-Net, a script), and edited by hand in a piano roll (§5.6).
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
- **Ethernet** (`net.ethernet(tesserkit::boards::gledopto())`, `/sys/network/ethernet`), as TesserKIT has it since 213ff08 and 0bc6ffa (its DESIGN §5.2):
  - `ethernet/enabled` *(p)* turns the MAC and PHY off; `wifi/mode` *(p)* says when the station joins: `fallback` (the default with a board: only while Ethernet has no DHCP lease, so a static or link-local address on a cable to one computer doesn't keep Wi-Fi off), `always` (both interfaces at once: a show network by cable, a phone over Wi-Fi), `never`.
  - **No DHCP on the cable** (a cable straight to a computer): a link-local address (169.254/16) after two unanswered requests, the controller found at `<hostname>.local`. lwIP's AutoIP, which this firmware turns on (`CONFIG_LWIP_AUTOIP=y`).
  - `ethernet/status/state` and `wifi/status/state` say why, in words ("waiting for Ethernet", `"X": wrong password?; retrying`, "connected, link-local address (no DHCP server)"...), and `wifi/failed` toasts a failed join.
  - A controller on a cable with no Wi-Fi settings opens its access point only after 15 s without an address. Wi-Fi settings aren't in release images: a board flashed from a release has its network saved once (captive portal, launcher over USB, or the Network page).

## 4. Architecture

```
 browser (TesserUI, x-midi ◀─ Web MIDI) ── HTTP / WebSocket ──▶ tesser::Api ◀── /sys: TesserKIT services (Art-Net writes /outputs/n/duty)
 USB serial ───────────────────────────────▶      │  /outputs (here)
                                                   ▼
   /midi/send ─▶ Midi ─▶ Output setters (under the API lock) ── plan() ──▶ Pwm ──▶ LEDC timer and channel per output
                  │  ▲
                  ▼  │ (replay)
               Recorder ◀── player task (esp_timer) ; recordings/*.mid in Files (LittleFS)
```

### 4.1 Pieces of this repository

| Piece | What | Depends on |
|---|---|---|
| `timing` | `plan()`: the clock, divider and resolution for a frequency (§5.2); `dutyCounts()` | nothing |
| `Pwm` | the platform interface: attach, timing, duty per output | `timing` |
| `pwm_esp` | `Pwm` on the classic ESP32's LEDC (§5.3) | ESP-IDF's LEDC driver and low-level layer |
| `Output`, `Outputs` | an output's settings, applied to its timer; the `/outputs` tree | TesserAPI, `Pwm` |
| `Midi` | MIDI messages onto the outputs: the key map, voices, pitch bend; the `/midi` tree (§5.5) | `Outputs` |
| `Recorder` | recording, replay and editing of recordings (§5.6); the `/recorder` tree | `Midi`, `ClipFiles`, `PlayerClock` |
| `smf` | Standard MIDI files: a recording to and from a type 0 file; types 0 and 1 read (§5.6) | nothing |
| `ClipFiles`, `PlayerClock` | the recorder's interfaces: its files (TesserKIT's `Files` in `firmware/main/clip_files.h`), its time and wake-ups (`platform/esp/player_esp.cpp`: a task woken by `esp_timer`) | |
| `x-midi` (`web/elements/midi.js`) | the custom element: Web MIDI in the browser, the keyboard and its map (§7) | TesserUI's custom element contract |
| `x-recorder` (`web/elements/recorder.js`) | the custom element: the transport, the recordings, the piano roll (§7) | TesserUI's custom element contract |
| `firmware/` | the device: TesserKIT services, pins, mounts, `begin()` | everything |

Everything but `pwm_esp` and the element is platform-free and host-tested; the element is tested in Chromium against the board (§10).

### 4.2 Tasks and locks

The tree's setters run in whichever task handles the request (httpd, the UART reader, TesserKIT's task for Art-Net and Timers), under TesserAPI's lock, and write LEDC registers directly: a few register writes, never waiting (§5.3). A duty that must wait for its channel's period is applied by `Pwm::poll()`, called from TesserKIT's tick (`dev.onTick`, every 20 ms) under the same lock.

One task of our own: the **player** (`photophonique-player`, priority 5, 6 KB of stack: a save goes through LittleFS). TesserKIT's tick is 20 ms and FreeRTOS's 10 ms, too coarse to replay a performance, so the player arms a one-shot `esp_timer` for the next event and sleeps until it fires, or until the recorder's schedule changes (a task notification). Each wake-up takes the API lock, plays what is due through `Midi` (the outputs' setters, as any client), saves a recording edited 2 s ago, and releases it. Nothing else touches an output.

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
- **Timing:** an item of a batch may be a pair `[message, ms]`: the message was played `ms` milliseconds (0–60000) before the request was sent. It is applied at once like the others; the recorder files it at its time (§5.6).

### 5.6 Recorder

What is played through `/midi/send` is recorded on the controller and replayed by it: no browser needs to stay open for a recording to play. Replay goes through `Midi` like a keyboard, so the key map and the bend settings in force at replay time apply: a remapped key replays on its new output.

- **Recordings** are Standard MIDI files in TesserKIT's `Files` (LittleFS), `recordings/<name>.mid`. A name has 1–28 characters, none of them `/` or a control character, and doesn't start with a dot (Files' temporary files do). Recordings survive a restart and a factory reset, but aren't in TesserKIT's settings backups (which hold the tree's persisted state only): download them.
- **In memory,** the current recording only: its notes (start, duration, pitch, velocity), its pitch bend points (time, value), its length, and its tempo and metre (for the grid and quantizing; the file's). Times are milliseconds from the recording's start. At most **1024 notes and 1024 bend points** (each read of the list is one JSON value, about 45 KB at most, which the WebSocket transport renders whole); a file of at most 64 KB. Each note and bend point has an `id`, given when the recording is loaded or the element added and never reused while it is, so edits name what they change even while other edits land.
- **Recording.** `record` with overdub off (or on an empty recording) starts a **new take**: a new recording named `Take n` (the first free n) becomes the current one, **armed**: its clock starts at the first note or bend heard, so the silence before the first note isn't recorded. With overdub on and a current recording with a length, `record` plays it from the start (or from where it plays) and records onto it (below). Notes on, notes off and pitch bend are recorded, mapped keys or not (the map may change); all notes off (CC 120 and 123) ends the notes it holds; other messages aren't recorded. A note still held when recording stops ends there. Pitch bend is thinned: a point less than 10 ms after the previous one replaces its value. Once a recording is full (1024 notes or bend points), what comes is played but not recorded, and `full` is announced (a toast).
- **The timing of what is heard** is the client's, not the network's: `x-midi` sends each message with how long ago it was played (`[message, ms]`), so a batch keeps its spacing (§5.5); the controller records it at `arrival − ms`. Network jitter between batches stays: a few milliseconds over Ethernet, more over Wi-Fi.
- **Stop** ends recording and replay. A new take ends at the stop: its length is the time from its first message, and it is saved. A take that heard nothing is dropped, its name freed. The notes replay was holding end (note offs for them only: what is played live keeps sounding, unless it is the same note).
- **Replay** (`play`, or `playing` set true) starts the current recording from its beginning, each event on its millisecond (the player's timer, §4.2), with the bend at the centre unless a bend point is at 0 (a recording without bend points leaves the bend alone). Note offs come before note ons at the same time, so a repeated note sounds twice. A note that runs past the length ends at the length. At the end, replay stops, or with **loop** on starts over (the bend back to its value at 0). `play` while playing restarts; `playing` set true while playing does nothing, so a gate held high doesn't retrigger.
- **Overdub** (on, with a recording that has a length): recording plays the recording and adds what is heard to it, at its position in the recording: with loop on, pass after pass, until stopped. A note held across the end ends at the end. Its length doesn't change.
- **Edits** (`edit`, from the piano roll or any client) remove, add and change notes and bend points by id; `length`, `bpm` and `beats` are written directly. An edit is validated whole before anything changes; ids already gone (another client's edit) are skipped. Edits are refused while recording. An edit during replay takes effect at once: what is past isn't replayed, a note held by replay that no longer covers the position ends, and a note now covering it waits for its next start. A recording edited is saved 2 s after the last edit, and when another is chosen, renamed or downloaded. A save holds the API lock while LittleFS writes (tens of milliseconds): edits during a show delay the next events by that much.
- **Quantize** (`quantize`): the start of the notes named (all if none) moves to the nearest multiple of `grid` beats at the recording's tempo, their durations kept; `ends` also moves their ends to the grid (at least one grid step long).
- **Choosing a recording:** `recording` (persisted, so it is the current one after a restart), or `cue` (1 to the number of recordings, in the list's order, plays it; 0 stops), made for Art-Net (§5.4) and other numeric clients. `playing` takes Art-Net's gate (a channel above a threshold). Timers (TesserKIT) call `play`, `stop` or `record`, or set `recording`.
- **Files:** each recording's `content` downloads its `.mid` and replaces it with an uploaded one (TesserUI's file widgets, over HTTP only: not over USB); `upload` adds a recording named after its file. Uploads are checked (a valid type 0 or 1 Standard MIDI file, within the limits) before anything is replaced. A file whose notes or bends exceed the limits is refused, not cut.

**Standard MIDI files** (`smf`):
- **Written:** type 0, one track, 480 ticks per quarter note; the tempo (`FF 51`), the metre (`FF 58`, a quarter note per beat), then notes (channel 1, note on and note off with velocity 64), pitch bend, a Marker named `end` at the recording's length, and the end of the track there or at the last note off if later (a track ends after its events: notes past the length are kept, as a DAW keeps notes outside its loop). Times are converted from milliseconds at the recording's tempo: within half a tick (about 0.5 ms at 120 BPM) of what was recorded.
- **Read:** type 0 and 1 (tracks merged), ticks per quarter note or SMPTE time; the tempo map is followed, the first tempo and metre kept as the recording's. Notes on every channel; a note on with velocity 0 is a note off; a note on already sounding ends the previous one; notes still sounding at the end of the file end there. Running status, system exclusive and meta events are read and skipped, except tempo, metre and the end of track. The length is the `end` marker if there is one, else the end of the last track, or of the last note if later.

### 5.7 Ensemble *(later)*

One controller, the **master**, plays the outputs of other controllers (its **nodes**) from MIDI, live and replayed by its recorder. Decided by Leo on 2026-10-13: the master decides, the show is on Ethernet (UDP) and NowTP is for testing, a shared clock later if it proves needed. Waits for TesserAPI's routes and streams and TesserKIT's configuration of them (§9).

- **Nodes** are ordinary controllers, found under `/peers` (TesserKIT's `UdpApi`; NowTp for testing). The master's node slots 1–9 each name one by its device id (station MAC), since names change. Without a master, a node plays its own keyboard and recordings as before.
- **The key map** reaches them: `outputs[n]` is 0 (none), 1–5 (this controller's outputs) or `10 · s + o`, output `o` of slot `s`'s node (11–15 … 91–95). Existing maps are unchanged.
- **The master decides:** voices and bend are computed on the master as now (§5.5). A node gets its outputs' settings, one patch of `/outputs` per node per batch (`{"3": {"on": true, "frequency": 440, "duty": 78}}`), through TesserAPI's `RemoteNode::stream()`: never waiting, one write in flight per node so they can't be reordered, and what waits merged into the next. The latest state wins: a note shorter than a round trip, or played during a lost datagram's retransmission (100 ms over UDP), may not sound, but the outputs always end as played.
- **Transports:** TesserKIT's global order (UDP, then NowTP) and per-node overrides; the route in use and the node's state shown per slot.
- **Access:** the master signs in to each node with an account it stores, once nodes have admin accounts (TesserKIT, §9); the token crosses the LAN in clear, as UDP does everything: the show's network is trusted (Leo, 2026-10-13).
- **A lost master** (gone from every transport, or restarted): each node turns off the outputs that master last wrote, and only those.
- **A node coming back:** the master sends it the state of the outputs it plays there.
- **Timing:** the nodes' writes leave one after the other, about a millisecond apart over Ethernet. Notes starting at the same instant everywhere (a shared clock, writes applied at a time) only if a scope shows audible skew (TesserAPI estimates two days' work).

## 6. The tree

Persisted nodes are marked *(p)*. Reads are public; writes to `/outputs` need `User`; TesserKIT's Auth leaves everyone Admin until an admin account exists. Nodes on the LAN reaching the tree over UDP (`/sys/udp`, on by default) are `User` without a token (TesserKIT's `peerAccess`), so they can play the outputs: the ensemble's master does (§5.7).

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
  send                  action: a packed message or an array of them (or of [message, ms] pairs), ui.hidden
  off                   action "All notes off"

/recorder               label "Recorder", ui.widget x-recorder, ui.script /ext/recorder.js
  state                 enum stopped, armed, recording, playing; read-only (announced)
  position              ms in the recording, read-only (announced at each start, stop, loop and edit; the element runs its own clock between)
  playing               boolean: true plays (unless playing), false stops. For Art-Net's gate
  recording *(p)        string: the current recording's name ("" none)
  cue                   integer 0–64: n plays the n-th recording, 0 stops
  loop *(p)             boolean (default false)
  overdub *(p)          boolean (default false)
  record, play, stop    actions
  length                ms, 0–14 400 000 (4 h): the recording's (written to its file)
  bpm                   20–300 (default 120), beats   1–16 (default 4): its tempo and metre
  notes                 read-only list ≤ 1024: {id, t (ms), d (ms), n (0–127), v (1–127)}, sorted by t
  bends                 read-only list ≤ 1024: {id, t (ms), v (-8192–8191)}, sorted by t
  edit                  action {remove: [id], set: [{id, t?, d?, n?, v?}], add: [{t, d, n, v}], addBends: [{t, v}]}
  quantize              action {ids?: [id], grid: beats (1/64–4), ends?: bool}
  full                  event, warn toast: the recording is full
  recordings/           read-only keyed list (key name): name, size (B), content (file: the .mid), rename, delete
  new                   action (name): an empty recording, made current
  upload                file: a .mid, named after its file name
```

## 7. Interface

TesserUI's generic renderer draws `/outputs` as a card per output: a toggle, a duty slider, frequency entry, a resolution slider and the applied values beside them, and `/midi` with its keyboard and the bend settings.

**Play** (a view, the first page): the keyboard, a row per output (on, the applied frequency, the duty), All off and All notes off, and the pitch bend settings. The keyboard forwards MIDI only while it is on screen, so this page is where the show is played.

**Record** (a view): the keyboard and the recordings' list (downloads and uploads) side by side, and the recorder (`x-recorder`) across the page under them (a wide card, TesserUI e87ff4d).

**`x-recorder`** (`web/elements/recorder.js`, plain JavaScript), over `/recorder`:
- **Transport:** the recording to work on (a menu), New, Rename, Delete; Record (red while armed or recording), Play, Stop, Loop, Overdub; the position, running between the controller's announcements on the browser's clock.
- **Piano roll:** time across (zoom with the buttons, or Ctrl or Cmd and the wheel; the zoom and the grid are remembered in the browser), pitch down the side (the pitches the recording uses and four more each way, two octaves at least), notes coloured by their key's output (`/midi/keys/outputs`), a playhead, the length as a handle to drag, and a beat and bar grid at the recording's tempo.
- **Editing:** click a note to select it (shift-click: add to the selection), drag on empty space to select a rectangle, Ctrl or Cmd+A all; drag notes to move them (snapped to the grid, unless Alt is held), drag a note's right edge to resize them; double-click empty space for a note there (a grid step long, the last velocity), double-click a note to delete it; Delete or Backspace deletes the selection, the arrows move it (by a grid step, a semitone, an octave with Shift up and down). The velocity of the selection is a field. Ctrl or Cmd+Z undoes the last edits made here (up to 30: each restores the notes and bends as they were, under new ids). A note just added is selectable once the controller has announced its id.
- **Bend lane** under the notes: the bend as a line through its points, dragged to move, double-clicked to add or delete.
- **Grid:** off, or a 1/4 (a beat), 1/8, 1/8 triplet, 1/16 (the default), 1/16 triplet or 1/32 note; **Quantize** snaps the selection's starts (or every note's) to it.
- Each gesture is one `edit` (split in several when longer than the request limit, 4 KB): shown at once, then reconciled with what the controller announces.

**`x-midi`** (`web/elements/midi.js`, plain JavaScript), over `/midi/keys`:
- **Web MIDI:** a choice of the computer's MIDI inputs (the last one chosen is remembered in the browser, else the first); notes, pitch bend and CC 120/123 forwarded to `/midi/send`, with one request in flight and what comes meanwhile sent as one batch, a newer bend replacing an unsent one. Each message waiting more than a millisecond goes with its age (`[message, ms]`), so the recorder keeps its timing (§5.6). Web MIDI needs Chrome or Edge and a secure page: the launcher on GitHub Pages, not the controller's own `http://` page, where the element says so and still edits the map. Chrome asks once for MIDI access.
- **The computer's keys** (as Ableton Live's computer MIDI keyboard): A W S E D F T G Y H U J K O L P ; ' play an octave and a half from C (C4 at first), Z and X move it an octave, C and V step the velocity through 1, 20, 40, 60, 80, 100 (the first), 120, 127. Notes go through the same queue as Web MIDI's, so they need no secure page: the controller's own `http://` page plays too.
  - **On** by default while no MIDI input is in use (no Web MIDI, no keyboard plugged in, or None chosen); the **Computer keys** button turns it on or off, and that choice is remembered in the browser.
  - **Physical positions** (`KeyboardEvent.code`): an AZERTY keyboard plays the same places. The keys on screen are labelled with the layout's letters where Chrome gives them (its keyboard layout map, on secure pages such as the launcher), QWERTY's elsewhere. The bar shows the range, the velocity and the keys that move them.
  - **Not while typing:** keys go to fields (here or anywhere on the page) and with Ctrl, Alt or Cmd, as usual. A key's release turns off the note its press played, even after an octave change; losing the window's focus, turning the keys off or leaving the page releases the notes held.
- **The keyboard:** 88 keys (A0–C8), scrolled to middle C, each coloured and numbered by its output; keys held on the computer's keyboard darken, keys sounding on the controller (`voices`) have a dot.
- **Editing:** click a key, shift-click for a range, or play keys with **Learn** on; then an output for all of them, and a frequency for a single key (empty or **Note's own**: the note's). Each edit writes the whole `outputs` array, or one element of `frequencies`.
- **Served** from flash at `/ext/midi.js` and `/ext/recorder.js` (7 KB and 10 KB gzipped; `web/build.mjs` gzips it into `src/photophonique/elements.h`, committed), and on GitHub Pages next to the launcher, which loads it through `window.TESSER.scripts`: an HTTPS page can't load it from the controller's `http://` address. The element defines itself once, whichever copy comes first.
- **Latency:** from the launcher by address, each batch is one HTTP request: a few milliseconds over Ethernet, more and less regular over Wi-Fi.

## 8. Persistence, first flash and updates

- TesserAPI persistence in NVS: one record per mount (`outputs`, `midi`, `recorder`), debounced 2 s. Stored backups and recordings (`recordings/`, §5.6) in `Files` (LittleFS, 384 KB).
- **Partition table:** TesserKIT's `tesserkit_4mb_files.csv` (two 1.75 MB OTA slots, LittleFS for `Files`, a core dump). The first flash is over USB (bootloader too: rollback is a bootloader setting), with `ballet-photophonique-esp32-factory.bin` at 0x0 or `idf.py flash`; updates are OTA after that. Gledopto's WLED is kept in `.local/` (gitignored) to restore with `esptool write-flash 0`.
- **Releases:** each push to `main` runs the host tests, then TesserKIT's reusable `firmware.yml` builds the firmware against the libraries' `main` branches (its ESP-IDF 5.5 image) and publishes `vX.Y.Z` (the last tag's patch raised) with `ballet-photophonique-esp32.bin` and `ballet-photophonique-esp32-factory.bin`. `[skip ci]` in a commit message releases nothing.
- **Updates:** `firmware.github("leokeba/ballet-photophonique", "ballet-photophonique-{target}.bin")`: the controller checks for a newer release a minute after coming online, then daily and on demand, and installs it from `/sys/firmware/github` (or the interface); a new image confirms itself after a minute online, else it rolls back (TesserKIT DESIGN §5.4). Uploads (`/sys/firmware/image`) and URLs work too.
- **Launcher:** the same workflow publishes TesserUI's launcher, built from the TesserUI the firmware has, to GitHub Pages: https://leokeba.github.io/ballet-photophonique/. It reaches a controller over USB (the API on UART 0) or by its address (`dev.cors("https://leokeba.github.io")`), with no network in common needed for USB.

## 9. Requests to other layers

| Request | Layer | For | Status |
|---|---|---|---|
| Ethernet: RMII with a LAN8720 (address, MDC, MDIO, clock mode and pin, power pin), beside Wi-Fi, under `/sys/network/ethernet` | TesserKIT | the GL-C-611WL's port, for a show network (§3) | done, TesserKIT 627088b (`boards::gledopto()`); works on the board over a cable (§10) |
| Turning `/sys/network/ethernet/static` off keeps the static address until a restart (seen 2026-10-12): the station, which waits while Ethernet has an address, then never joins. TesserKIT §5.2 says `static` is applied at once | TesserKIT | moving a controller from a cable to Wi-Fi | done, TesserKIT 213ff08 |
| More flexible interfaces: Ethernet and the station each on or off; a policy instead of the fixed "Ethernet first" (`ethernet first`, `both`, `wifi first`), so a show cable and Wi-Fi can serve at once; a static address on an isolated link not keeping Wi-Fi off; a fallback when the cable has no DHCP (link-local or a configured address); why an interface is in its state, in the tree (`waiting for Ethernet`, `wrong password`, `network not found`) | TesserKIT | Leo, 2026-10-12: a board couldn't be moved from its cable to Wi-Fi without serial | done, TesserKIT 0bc6ffa (Leo chose a station mode `always`, `never`, `fallback` over separate switches, plus `ethernet/enabled`; no "Wi-Fi first"); AutoIP on here; not yet tried on the board |
| ArtPollReply's MAC is the base MAC (…:bc) when the node is reached over Ethernet (…:bf), whose address the reply gives | TesserKIT | consoles listing nodes by MAC | done, TesserKIT 0bc6ffa (`dev.addressMac()`) |
| More than 8 Art-Net mappings (a duty and a frequency per output is 10) | TesserKIT | §5.4 | to ask |
| A subscription over plain HTTP: one long `GET` streaming notification envelopes, one per line, with keep-alives (Leo, 2026-10-12, "option 1": events only; full parity with stream ids not asked) | TesserAPI | the launcher on Pages reaching a controller by address polls over HTTP and never hears events: no toasts, no events log (§7) | done, TesserAPI 20d403e (TesserAPI §10.2, "Streamed subscriptions"; TesserKIT unchanged), not yet on hardware: `GET <api><path>?sub&...` answers `application/x-ndjson`, a bare line after 15 s idle; at most 2 streams (`HttpServer::setMaxStreams`), each one of httpd's 7 sockets. A stream below a remote node (forwarded) is `not_allowed` for now |
| Over HTTP, the tree's events read from that stream, values still polled | TesserUI | as above | done, TesserUI 2bfebb6 (not pushed as of 2026-10-12): `GET <api>/?sub&interval=60000&changes=false&snapshot=false`, NDJSON, reconnecting; anything else is a refusal, so no toasts until the firmware has TesserAPI's part. A node allows 2 streams (`HttpServer::setMaxStreams`): a third open page gets no toasts. The launcher on Pages picks it up once TesserUI's `main` has it. Seen while asking: Chrome 154 opens `ws://` to a private address from the HTTPS launcher once local network access is granted (a test server, not a controller), which TesserUI's `resolve()` assumes it can't; Leo's call |
| A view card spanning the page's width (or a column count for view pages): the Record view's piano roll shares its page with the keyboard (x-midi forwards only while on screen), so it can't be a custom page, and gets a third of a wide window | TesserUI | the recorder (§7) | done, TesserUI e87ff4d (not pushed as of 2026-10-13): a card item's `"wide": true`, a row of its own across the grid (`page.hint("wide", true)` right after `card()`; older renderers ignore it). Used by the Record view; checked on the second board with a renderer built at e87ff4d |
| Wi-Fi joins time out before DHCP binds: `Network`'s 10 s join timeout (`kConnectTimeoutMs`) covers the scan, association, DHCP and lwIP's address conflict detection (`CONFIG_LWIP_DHCP_DOES_ACD_CHECK`, ESP-IDF's default: three ARP probes and a 2 s wait, about 5 s). Seen 2026-10-13 on the second board with a Freebox that acknowledges the third DHCP request (2.3 s): associated at 2.9 s, bound at about 10.3 s, dropped at 10 s, "no answer", every attempt | TesserKIT | joining home and venue routers | done, TesserKIT b207f3e (not pushed as of 2026-10-13): 10 s to associate, then 20 s for the address; `wifi/status/state` says "getting an address from "X"" meanwhile, "no address (DHCP)" if it fails. On the second board, by DHCP, first attempt: associated 2.9 s after `connect()`, address 9.8 s later (12.7 s in all, 20 s after a reset) |
| A PWM output part with runtime frequency and resolution, if another device needs one | TesserMotion | `Pwm` and `Output` | not asked: TesserMotion's `Pwm` attaches at a fixed frequency; this repository's stays here until a second user |
| Several transports per node, chosen by a priority order (global, overridable per node), failing over and back; writes to a node that never wait, never reorder and fail at once when it is offline; an event on the receiving side when a client is lost; a set's handler learning the writer's device; a node found by its MAC; later, a shared clock | TesserAPI | the ensemble (§5.7) | asked 2026-10-13 (tesserapi-3d), in progress: routes on `RemoteNode` (a second transport's announcement adds a route; `setDefaultRouteOrder`, `routeOrder()`, `route()`), `RemoteNode::stream(path, body)` (one set in flight per node, queued ones merged as JSON merge patches; forwarded requests below an offline node now fail at once), `DatagramEndpoint::onPeerLost` and `tesser::watchDevices`. Writer's device and lookup by MAC asked after. Shared clock: not now (about 2 days; worth it only with measured skew) |
| The transport order and per-node overrides in the tree, each node's route, state and round trip, nodes added by address; a device's credentials on its nodes once they have an admin account; NowTp beside Ethernet; a Wi-Fi power-save setting | TesserKIT | the ensemble (§5.7) | asked 2026-10-13 (tesserkit-c4), planned on TesserAPI's routes: `/sys/peers` (the order persisted, per node an override and a declared address, a read-only status with a toast when a node is lost; keyed by MAC asked); the master signs in to each node's `/sys/auth/login` with a stored account (a default, per-node overrides) and presents the token on every route (tokens in clear: UDP's trusted-LAN model, no pairing over UDP); `/sys/network/wifi/powerSave` (none, min, max; none refused while Bluetooth runs). NowTp beside Ethernet works per the code (the radio in station mode, started on an Ethernet lease, on `/sys/nowtp/channel`, the same on every node without an anchor), untried on hardware. "Start at T": later (on Ethernet, an NTP server on the show LAN or a UDP time sync) |
| Reordering an array (the transport order); choosing a node among `/peers` with its online state, as `ui.options` does for a keyed list | TesserUI | the ensemble (§5.7) | asked 2026-10-13 (tesserui-f4) |

## 10. Testing

1. **Host tests** (CMake, ASan, UBSan, `build/photophonique_tests`, 44 cases): `plan()` against hand-computed dividers (ESP-IDF's 5 kHz, 13-bit example), fractional frequencies, the resolution lowered and raised, a sweep of every frequency and resolution landing within one divider step; duty rounding; outputs on the fake PWM (stored until begun, rescaled duty, All off, a refused pin); the tree (ranges, patches in either order, persistence and restore).
2. **The recorder** (host): Standard MIDI files written and read back, a file from elsewhere (type 1, running status, a tempo change, SMPTE time, velocity 0 offs), malformed files refused; recording with times from the batch's ages, the armed take starting at its first note, bend thinning, the limits; replay on a fake clock (each event at its millisecond, offs before ons, loop, the end, stop releasing only replay's notes), overdub across the loop, edits during replay, quantize, saving after 2 s, choosing, renaming, deleting and uploading recordings through the tree, `cue` and `playing`.
3. **MIDI in the browser** (`test/midi/`): `keyboard.py` opens a virtual CoreMIDI source (python-rtmidi), `serve.py` serves the launcher page, TesserUI's bundle and `midi.js` on localhost (a secure context) with `/api` proxied to a controller, and `e2e_midi.mjs` drives Chromium (Playwright, MIDI permission granted): the element finds the keyboard, notes reach their outputs with their frequency and velocity's duty, a bend moves two sounding outputs, a burst of three notes arrives batched, the keyboard shows the sounding keys, note off and all notes off, then the map edited by clicks, a shift-click range and a frequency typed and reset. 13 checks (the computer's keys off while a MIDI input plays, too), passing on 2026-10-12 against the second board over Ethernet, from localhost and from the published launcher itself (`https://leokeba.github.io/ballet-photophonique/?api=http://192.168.3.50/api`, v0.1.1: HTTP polling, CORS, the element from Pages). There the console reports the controller's own copy of the script blocked (CORS doesn't cover `/ext/`): expected, the page's copy defines the element.
4. **The piano roll** (`test/recorder/e2e_recorder.mjs`), on the launcher page with x-recorder, against a controller by address (`test/midi/serve.py ... web/elements/midi.js web/elements/recorder.js`, which answers `?sub` with a plain value so the page polls: it relays whole responses only) or over USB (`test/recorder/serial_bridge.py`: `/api` bridged to the serial API, no subscriptions, the page polls): a double click adds a note on the grid, dragging moves it, its edge resizes it, the velocity field, Delete, Quantize and its undo, the arrows, a rectangle selection, a bend point, the end's handle, Play to the end, a take from the computer's keys (an octave down, unmapped keys: no output turns on) with its timing, Rename and Delete. 19 checks, passing on 2026-10-13 against the second board over USB. Over the bridge, a request can wait hundreds of milliseconds behind the page's polls: within a batch the spacing is the keys', between requests the bridge's latency adds (§5.6).
5. **The computer's keys** (`test/midi/e2e_typing.mjs`), on the controller's own `http://` page (no secure context, the element from flash, live over WebSocket): the element says there is no Web MIDI and turns the keys on, C4 is labelled A, A plays C4 at velocity 100, C lowers the velocity, X raises the octave, a key released after Z still releases its note, typing in the frequency field and Ctrl+A play nothing, losing focus releases a held key, and the button turns the keys and their labels off. 17 checks, passing on 2026-10-12.
6. **On the board:** v0.1.0, the first release (ESP-IDF 5.5), found and installed from `/sys/firmware/github` over Wi-Fi on 2026-10-12, settings kept, confirmed after a minute online. Ethernet (2026-10-12, a second GL-C-611WL, MAC 20:50:0d:1b:02:bc, flashed with v0.1.0's factory image, cabled straight to a Mac's USB adapter, no DHCP on the link): link up at 100 Mbit/s, a static address set over USB serial (`/sys/network/ethernet/static`), then over the cable: ping (1.3 ms), the API and the interface, outputs set, ArtPoll answered with the Ethernet address and ArtDmx writing a duty, a 1.35 MB upload in 6.5 s (208 KB/s) booted and confirmed online on the cable alone, and a restart back on the cable in 3 s without its access point coming up (no Wi-Fi saved). Before the address was set, the access point came up after 15 s and went down 30 s after the address, as TesserKIT specifies. Not tried: unplugging the cable to fall back to Wi-Fi, DHCP over the cable. By hand on 2026-10-12, outputs off: writes over HTTP and serial across both clocks, both speed groups and periods up to 1000 s, with no reset; a stored 0.5 Hz state boots. **UDP** (2026-10-13, second board, TesserKIT 4b9a910, ESP-IDF 6.1, on Ethernet by DHCP behind a Freebox): with `/sys/udp`, 113 KB of heap free (109 KB at the lowest), 1.50 MB image; the two TesserKIT devkits on the Freebox's Wi-Fi found under `/peers` within seconds. A read forwarded to them through the board took 54 and 101 ms longer than a direct one (median of 40; RSSI -68 and -83 dBm, the stations' modem sleep likely), which Wi-Fi nodes would add to live playing. Ethernet to Ethernet is not measured yet (one board). *(later)* `test/e2e_outputs.py`, and a scope on the terminals: frequency and duty against `applied/`, at the limits of each clock, and a duty change at low frequency arriving within two periods.

## 11. Roadmap

| Phase | Content | Acceptance |
|---|---|---|
| 0 *(built)* | Repository, outputs, tree, host tests, firmware with TesserKIT's services | Host tests green; the firmware builds (1.09 MB, 38 % of the slot free), boots on the board, and takes writes at every frequency and resolution without a reset |
| 1 | Outputs measured with a scope, Art-Net from a console | Each output's frequency and duty within a divider step of `applied/` |
| 2 *(built)* | Ethernet (TesserKIT), releases from GitHub, the launcher on Pages | The controller on a show network without Wi-Fi, updated from a release. Built: 1.27 MB on ESP-IDF 6.1-dev (27 % of the slot free), 1.29 MB on 5.5 |
| 3 | The Function button | |
| 4 *(built)* | MIDI: the key map, voices and bend on the controller; `x-midi` and the Play view; the computer's keys | Played from a keyboard through the Pages launcher. Released in v0.1.1; tested with a virtual keyboard from the Pages launcher (§10); not yet with a real keyboard |
| 5 *(built)* | The recorder: takes, replay with loop and overdub, Standard MIDI files in `Files`, the piano roll, cues for Art-Net and timers (§5.6) | A performance recorded from a keyboard, replayed by the controller alone with its timing (within a few milliseconds of the player's timer, on a scope or a logic analyser), edited, downloaded into a DAW and uploaded back. Built 2026-10-13 (1.42 MB on ESP-IDF 6.1-dev, 22 % of the slot free): host tests, the serial API and the piano roll on the second board over USB; a take of unmapped keys over serial landed at the times played (a batch's ages within a few ms). Not yet: replay timing on a scope, a real keyboard, a DAW round trip, the Pages launcher |

| 6 | The ensemble (§5.7): a master playing other controllers' outputs from MIDI, over UDP and NowTP by priority | A take replayed by the master on two controllers over Ethernet, each output within a few ms of the other on a scope; a node unplugged mid-note and the master restarted, every held output turning off |

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
- **A recorder on the controller, in its filesystem** (Leo, 2026-10-12): recordings replay without a browser and are files in TesserKIT's `Files`. They hold MIDI messages, replayed through the key map in force (not the outputs' settings as heard); Standard MIDI files, so a DAW opens and writes them; a piano roll for editing; loop, overdub, quantize, and replay started from the tree (timers, Art-Net). A new take never replaces a recording: it is a new one.
- **An ensemble, the master deciding** (Leo, 2026-10-13): one controller plays others' outputs from MIDI and its recorder. Its key map names other controllers' outputs, and it sends them output settings, not MIDI, so the map is in one place and the nodes stay ordinary controllers. The show on Ethernet (UDP), NowTP for testing, the transport order global with per-node overrides, a shared clock later if needed. Transports, routes and their configuration are the libraries' (§9).
