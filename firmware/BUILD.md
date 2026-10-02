# Build and flash

Arduino IDE 2.x with the **ESP32 core 3.3.7** (Espressif).

## 1. Libraries

Copy both folders from `libraries/` into your Arduino libraries directory
(`Documents/Arduino/libraries/` on Windows):

- `BanglaEPD`
- `BanglaUTF8`

From the Library Manager, install:

| Library | Version used |
|---|---|
| GxEPD2 | 1.6.9 |
| Adafruit GFX Library | 1.12.6 |
| Adafruit BusIO | 1.17.4 |
| RTClib | 2.1.4 |
| DFRobotDFPlayerMini | 1.0.6 |
| Adafruit Fingerprint Sensor Library | 2.1.4 |
| ESP Async WebServer | 3.12.1 |
| Async TCP | 3.5.0 |

## 2. Board settings

Open `integration_8/integration_8.ino`, then set all of these under **Tools**:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Disabled** for the UART port · **Enabled** for the native USB port |
| PSRAM | OPI PSRAM |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Upload Speed | 115200 |

The partition scheme must be a **FATFS** one, or the event log cannot mount.

The board exposes two USB ports and they need opposite `USB CDC On Boot`
settings. The UART bridge is the more reliable of the two for flashing: it
drives reset and boot with real hardware lines, so it can recover a board whose
firmware has stopped servicing USB.

## 3. Flash

Expect roughly 65% of program storage and 17% of dynamic memory. The build
takes a few minutes — the Bengali font tables are about 3.8 MB of source.

If an upload fails with a timeout or "chip stopped responding", simply upload
again.

## 4. Audio

Copy `audio/MP3/` to the root of a FAT32 card (32 GB or smaller) and fit it to
the DFPlayer. At boot the serial log reports what it found:

```
Files detected: 17  (expected 17)
```

Missing files are reported and are not fatal — the screen carries the same
instruction, and the buzzer sounds for every reminder regardless.
