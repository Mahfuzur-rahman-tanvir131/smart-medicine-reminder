# Setup and tests

Moved out of README.md, which is now just the user manual.
Flashing the board, the full test checklist, and where the project stands.

# PART 1 — Flashing it

### Open
Arduino IDE → **File → Open** →
`D:\academic 4-1\EEE 416\Project\project_gopal\integration_8\integration_8.ino`

### Set these six in the Tools menu

| Tools → | Set to | If wrong |
|---|---|---|
| Board | ESP32S3 Dev Module | — |
| Port | **COM6** | can't upload |
| USB CDC On Boot | **Enabled** | Serial Monitor blank |
| PSRAM | **OPI PSRAM** | board misbehaves |
| Flash Size | **16MB (128Mb)** | **boot loop** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** | event log fails |

> Other USB socket? Then **USB CDC On Boot = Disabled**. Opposite settings.

### Upload
Click **→**. Wait for *"Hard resetting via RTS pin"*.
Won't upload? Hold **BOOT**, tap **RST**, release **BOOT**, upload again.

### A note on the first boot after flashing

The compartments stay shut from the instant the chip starts. If you ever see a
lock click open at power-up, that is a fault - tell me, do not work around it.

### Check it booted
**Tools → Serial Monitor**, **115200 baud**, press **RST**.
Everything should say **OK**, ending with `INTEGRATION-9 READY`.

Then look at the screen: it should show **পরবর্তী** with a name, a time and a
medicine — in Bangla.

---


# PART 3 — Testing it works

### Basics
- `P` lists 4 slots · `N` updates the screen · `L` shows the log
- Type `L`, press **RST**, type `L` again → one more `SYSTEM_BOOT` line

### A real dose
Set a slot to **2 minutes from now**, walk away, follow the 7 steps above.

### The failure cases — these matter most

| Test | Do | Should happen |
|---|---|---|
| **Wrong person** | ঠাকুমা's finger at দাদু's dose | **Nothing opens.** **ভুল ব্যবহারকারী** |
| **Stranger** | Unregistered finger | Same |
| **Wrong time** | Any finger, no dose running | **Nothing happens at all** |
| **Missed** | Ignore it 10 min | Logged as missed |
| **Not confirmed** | Open it, don't press SELECT | After 2 min: **নিশ্চিত হয়নি** |
| **Override** | UP+DOWN held 3 s | Opens, logged as OVERRIDE |

**"Wrong time" is the one to show your examiner** — it proves an enrolled
finger alone can't open the box.

### Power saving + Wi-Fi
Hold **UP+DOWN 3 s** → screen becomes a clock, Wi-Fi drops.
Hold **BACK 3 s** → back to **পরবর্তী**, Wi-Fi returns.

### The dashboard
1. Normal mode → join `Oshudh-Box` / `oshudh1234`
2. The page should open by itself. If not, open `192.168.4.1`
3. **সময়সূচি** → change a time → **সংরক্ষণ** → asks for PIN `1234`
4. Check the e-paper updated, and that `P` on serial shows the new time
5. **ইতিহাস** → your change appears as `SCHEDULE_EDIT`
6. Turn Wi-Fi off (UP+DOWN 3 s) → **a dose must still work normally**

Step 6 is the one that matters: it proves the medicine flow does not depend on
the network.

### Fingerprints
1. যন্ত্র tab → আঙুলের ছাপ → **বদলান** next to ঠাকুমা
2. **Give দাদু's finger at the পুরানো ছাপ দিন step.** It must refuse. This is
   the test that matters — without it, the PIN alone would be enough to steal
   someone's access.
3. Start again, give ঠাকুমা's finger, then follow the six prompts
4. Check ঠাকুমা's **new** finger opens her dose
5. Check her **old** finger no longer does
6. ইতিহাস → `FINGERPRINT_CHANGED` appears, and a Telegram message arrives

### The caretaker
1. আঙুলের ছাপ → **যুক্ত করুন** next to কেয়ারটেকার
2. It asks for a finger first — দাদু or ঠাকুমা authorises
3. Enrol the caretaker's finger through the six prompts
4. Wait for a dose belonging to **দাদু**, then open it with the **caretaker's**
   finger. It must open.
5. ইতিহাস → `CARETAKER_OPEN`, and a Telegram message arrives

### Browsing and skipping
1. Idle screen → press **UP** → today's doses appear, one per press
2. Step to a dose later today → it says **পরবর্তী করতে চাপুন**
3. Press **SELECT** → it asks for a fingerprint
4. **Give the WRONG person's finger first.** It must refuse with
   **ভুল ব্যবহারকারী** and then ask again. This is the test that matters.
5. Now give the right finger → **পরবর্তী নির্বাচিত**
6. **ইতিহাস** tab → a `DOSE_SKIPPED` row appears
7. A Telegram message arrives naming what was skipped
8. The idle screen now shows the chosen dose as **পরবর্তী**
9. Let it run to that time → the skipped dose must **not** fire, the chosen one must

### Telegram (only if you switched it on)
1. যন্ত্র tab → check **ইন্টারনেট** says **যুক্ত**
2. **একটি পরীক্ষা মেসেজ পাঠান** → both phones get a message
3. Let a dose go past without touching it → a ❌ message arrives within a minute
4. Turn on power saving (UP+DOWN 3 s) and let another dose go past → the alert
   still arrives. This is the one worth checking: it proves the box wakes its
   own radio.
5. Set **দিনের হিসাব কখন পাঠাবে** to the next hour and wait → the day's summary
   arrives.

On serial you will see `[TG] queued`, then `[TG] sent, 0 left`.

### Wi-Fi comes back cleanly
Hold **UP+DOWN 3 s** (Wi-Fi off) → hold **BACK 3 s** (Wi-Fi on) → rejoin from
your phone. **No RST press should be needed.**

### PIN and restart
যন্ত্র tab → change the PIN → power-cycle → confirm the new PIN is still in
force. Then try **পুনরায় চালু** and rejoin after 20 seconds.

---


# PART 5 — Status

**Working:** Bangla screens · right person only · right time only · wrong-finger
message · event log survives restarts · course duration · override · power-cut
recovery · watchdog · RTC wake alarm · power saving · buzzer.

**Voice — seventeen tracks.** The original ten, plus seven added for changing
a fingerprint and moving a dose forward. All Mahiya (Female) from Narakeet,
192 kbps 48 kHz mono, listed in `audio/VOICE-SCRIPT.md`.

Copy the whole of `audio/MP3/` to `/MP3/` on the card. If tracks 0011–0017 are
missing the box still works — the e-paper carries the same instruction and
nothing waits on the audio.

**Voice — intermittent, now self-healing.** The SD card is sometimes seen at
power-up and sometimes not, with identical firmware. That points at a marginal
card socket rather than software.

The firmware no longer gives up at boot. If the card is missing it keeps
retrying quietly every 30 seconds, resets the audio module every third attempt
to force a re-mount, and tries once more just before a reminder speaks. When the
card reappears you will see:

```
[AUDIO] SD card appeared - 10 files. Voice is working again.
```

The buzzer always alerts regardless, so a reminder is never silent.

**If it stays missing:** reseat the card firmly, and check it is **FAT32** (not
exFAT) and 32 GB or smaller. Press **`C`** for a full diagnosis.

Volume is **25**, the value proven to work on this hardware.

**Wi-Fi + local dashboard:** done — see PART 2.

**Telegram alerts:** done — optional, two people, alerts on missed doses and a
summary every evening. Set up from the dashboard; see PART 2.

**No cloud dashboard.** Deliberately dropped. The evening Telegram summary
delivers the history without an account to make, a password to forget, or a
server to keep paying for.
Reed sensors: code written and switched off — set `HAS_REED_SENSORS 1` once
GPIO38 is wired.