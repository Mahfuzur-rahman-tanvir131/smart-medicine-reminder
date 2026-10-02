# Smart Medicine Reminder — ঔষধ যন্ত্র

A biometric medicine box for two elderly users, with a fully Bengali interface.

At the scheduled time it sounds a buzzer, speaks in Bangla and unlocks **only the
correct compartment**, and **only** for the fingerprint of the person that dose
belongs to. Every dose is recorded. Nothing in the user's path requires a phone,
an account, or an internet connection.

EEE 416 Project · Department of EEE, BUET

<p align="center">
  <img src="hardware/pcb-3d.jpg" width="640" alt="PCB, 3D render">
</p>

---

## Design intent

Most pill reminders beep and hope. In an elderly household the problem is not
the reminder — it is the **wrong medicine being taken**, and nobody able to say
afterwards whether it was taken at all. Three rules follow:

- **Only the scheduled person, only during their dose window, opens that
  compartment.** An enrolled but wrong fingerprint opens nothing; a correct
  fingerprint outside a dose window opens nothing.
- **Every dose ends in a recorded outcome** — taken, missed, or unconfirmed.
- **The device must be useful with no network at all.** Reminders, fingerprint,
  locks, screen and log are entirely local; the network is optional everywhere.

Everything the user sees or hears is Bengali.

---

## Features

| | |
|---|---|
| **Scheduling** | 4 compartments, independent times, per-dose medicine name and course length |
| **Authentication** | R307S fingerprint — two residents and a caretaker with override rights |
| **Locks** | 4 solenoids, 400 ms pulse, one at a time, software interlocked |
| **Display** | 1.54" e-paper, Bengali, partial refresh |
| **Voice** | 17 Bengali prompts over DFPlayer Mini |
| **Logging** | Append-only CSV on 9.8 MB of on-chip flash — roughly 34 years of doses |
| **Dashboard** | Bengali web UI served by the device's own Wi-Fi access point, no router needed |
| **Alerts** | Optional Telegram — missed doses as they happen, plus a daily summary |
| **Power** | Light sleep between doses, hardware RTC alarm wake |

### Dose sequence

```
buzzer ──▶ "দাদু, ঔষধ গ্রহণের সময় হয়েছে"  ──▶ আঙুলের ছাপ দিন
                                                      │
                     wrong finger ◀───────────────────┤
                   (nothing opens,                    │ correct finger
                    logged, voiced)                   ▼
                                            compartment unlocks
                                                      │
                            ┌─────────────────────────┴──────────────┐
                        SELECT pressed                      120 s, nothing
                            ▼                                        ▼
                      ঔষধ গ্রহণ সম্পন্ন                      ঔষধ গ্রহণ নিশ্চিত হয়নি
                          TAKEN                                 UNCONFIRMED
```

No fingerprint within 10 minutes → **MISSED**, logged and alerted.

---

## Bengali rendering

Bengali is a shaping problem, not a font problem: `ক` + `্` + `ষ` must become the
single conjunct `ক্ষ`, not three glyphs side by side. HarfBuzz is far too heavy to
run on an ESP32.

The solution is to **shape offline and render directly**. HarfBuzz and FreeType
pre-shape and rasterise every cluster the interface needs into a flat table —
**74 single glyphs and 4,247 pre-shaped clusters** at 28 px, about 0.64 MB in
flash. At runtime the renderer walks each string longest-cluster-first, matching
sequences of up to 8 codepoints.

The result is correct conjuncts, reph, র-ফলা and য-ফলা on a microcontroller, with
no shaping engine on the device. See [`libraries/BanglaEPD`](libraries/BanglaEPD).

---

## Hardware

**ESP32-S3-WROOM-1** (N16R8 — 16 MB flash, 8 MB OPI PSRAM)

| Function | Pins |
|---|---|
| I²C — DS3231 RTC | SDA 8, SCL 9, INT 21 |
| E-paper — 1.54" SSD1681 | CS 10, MOSI 11, SCK 12, DC 13, RST 14, BUSY 15 |
| DFPlayer Mini | RX 18, TX 17 |
| R307S fingerprint | RX 2, TX 1 @ 57600 |
| Buttons | UP 4, DOWN 5, SELECT 6, BACK 7 |
| Solenoid locks | C1 39, C2 40, C3 41, C4 42 — active HIGH |
| Buzzer | 16 |

Solenoids are driven by LR7843 modules with flyback diodes and 10 kΩ pulldowns,
and the firmware holds all four low from the first instruction in `setup()`, so
the compartments stay shut from the instant the board powers on.

<p align="center">
  <img src="hardware/pcb-layout.jpg" width="700" alt="PCB layout">
</p>

Schematics: [overview](hardware/schematic-overview.jpg) ·
[power and locks](hardware/schematic-power-locks.jpg) ·
[low voltage](hardware/schematic-low-voltage.jpg)

---

## Security model

- **There is no remote unlock.** Not disabled — absent. No endpoint, no serial
  command, no message opens a compartment. It needs a finger on the sensor.
- **The dashboard PIN gates every write.** Reading is open; changing a schedule,
  the clock or a fingerprint is not.
- **Replacing a fingerprint requires the old one first**, so the PIN alone cannot
  transfer someone's access. Adding the first caretaker requires a resident's
  fingerprint.
- **Telegram is outbound only.** Nothing on the internet can open a connection to
  the device.
- **Overrides are never silent** — caretaker unlocks, button overrides and skipped
  doses are all logged and alerted.

---

## Repository layout

```
firmware/integration_8/   the firmware — one sketch, plus the dashboard page
libraries/                BanglaEPD + BanglaUTF8, the Bengali rendering layer
audio/                    17 Bengali voice prompts, and the script that made them
hardware/                 PCB and schematics
docs/                     user manual, build settings, test checklist
```

- **[docs/USER-MANUAL.md](docs/USER-MANUAL.md)** — operating the device
- **[firmware/BUILD.md](firmware/BUILD.md)** — libraries, board settings, flashing
- **[docs/SETUP-AND-TESTS.md](docs/SETUP-AND-TESTS.md)** — verification checklist

---

## Build

Arduino IDE 2.x with ESP32 core 3.3.7. Copy `libraries/BanglaEPD` and
`libraries/BanglaUTF8` into your Arduino `libraries/` folder, then open
`firmware/integration_8/integration_8.ino`. Board settings are in
[firmware/BUILD.md](firmware/BUILD.md).

Copy `audio/MP3/` to the root of a FAT32 SD card for the voice prompts. The
device works without them — the screen carries the same instruction and the
buzzer always sounds.

---

## Scope

Implemented: Bengali screens, voice and dashboard · per-user fingerprint
authorisation with caretaker override · course duration and final-dose handling ·
on-device dose browsing and rescheduling · fingerprint enrolment · persistent
event log · power-cut recovery · Telegram alerts · battery power saving.

Reed switches for door-close detection are supported in firmware behind
`HAS_REED_SENSORS` but are not fitted, so an unconfirmed dose is determined by
the confirm button rather than a door sensor.

---

## Authors

Students 2106131, 2106135, 2106136, 2106147
Department of Electrical and Electronic Engineering, BUET

Licensed under the [MIT License](LICENSE).

> The Bengali glyph tables in `libraries/BanglaEPD/BanglaFont.cpp` are generated
> with HarfBuzz and FreeType from a Bengali typeface. The MIT licence covers this
> project's own code; check the upstream typeface's licence before reusing those
> tables elsewhere.
