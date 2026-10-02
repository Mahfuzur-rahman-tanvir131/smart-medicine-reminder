# ঔষধ যন্ত্র — User manual

A medicine box for দাদু and ঠাকুমা. Four compartments, each locked. At the
right time it beeps, speaks in Bangla, and opens **only** the right
compartment, and **only** for the right person's fingerprint.

Everything the box shows and says is in Bangla. It works with no internet and
no router at all.

> Flashing the board and the test checklist are in
> [SETUP-AND-TESTS.md](SETUP-AND-TESTS.md).

---

## The four buttons

| Button | What it does |
|---|---|
| **UP / DOWN** | Step through today's doses · change a value |
| **SELECT** | Confirm a dose was taken · choose the dose on screen |
| **BACK** | Go back · **hold 3 s** to cancel a reminder · **hold 3 s** to leave power saving |
| **UP + DOWN together** | **Hold 3 s** to turn power saving on |

---

## The two modes

### Normal mode

Screen shows **পরবর্তী** — who is next, when, and which medicine.
Wi-Fi is **on**, so the dashboard works.

### Power-saver mode

Screen shows **only the clock** and **সাশ্রয়ী মোড**. Wi-Fi is **off** and the
box dozes between doses.

**Reminders still fire exactly on time.** The clock chip keeps running and
wakes the box. When a dose is due it wakes by itself and behaves normally, then
returns to the clock face when the dose is finished.

| To do this | Do that |
|---|---|
| Power saving **on** | Hold **UP + DOWN** together, 3 seconds |
| Power saving **off** | Hold **BACK**, 3 seconds |

The clock repaints every 2 minutes, and only the time line — the screen does not
flash.

> While USB is plugged in the chip does not truly sleep; only the screen
> changes. Sleeping would cut the USB connection and freeze the Serial Monitor.
> On battery it sleeps properly.

---

## What a normal dose looks like

| | |
|---|---|
| 1 | Buzzer beeps |
| 2 | Screen: **ঔষধ গ্রহণের সময়** with the name and time, and the voice says so |
| 3 | Screen adds **আঙুলের ছাপ দিন** |
| 4 | That person puts their finger on the sensor |
| 5 | Click — that compartment opens. Screen: **খোপ খুলেছে** |
| 6 | Take the medicine, press **SELECT** |
| 7 | Screen: **ঔষধ গ্রহণ সম্পন্ন** |

If nobody comes, it is recorded as **ঔষধ গ্রহণ করা হয়নি**.
If the box opens but SELECT is never pressed, **ঔষধ গ্রহণ নিশ্চিত হয়নি**.
Either way a Telegram message goes out, if you have set that up.

---

## Seeing today's doses on the box

On the idle screen press **UP**, **DOWN** or **SELECT**. The screen steps
through today's four doses — who, what time, which medicine, and whether it is
still coming or already done.

**BACK** returns to normal, and it gives up on its own after 30 seconds.

Keep going past the fourth dose and there is a **fifth screen**,
**সময় ও শব্দ**: the clock, the date, and the volume.

---

## Changing the volume

Browse to **সময় ও শব্দ** and press **SELECT**.

- **UP / DOWN** — louder or quieter, 0 to 30
- **SELECT** or **BACK** — save and return

Only the digits repaint, so the screen does not flash. The dial itself is
silent; you hear the new level at the next reminder. It starts at **25** and
survives a power cut.

---

## Skipping a dose and moving a later one forward

দাদু is going out and wants his evening medicine now, giving up the dose in
between.

1. **UP / DOWN** until his later dose is on screen. It says
   **পরবর্তী করতে চাপুন**.
2. Press **SELECT**.
3. The screen asks for a fingerprint. **দাদু gives his own finger.**
4. **পরবর্তী নির্বাচিত** — that dose is now next, and the skipped one is
   recorded as given up.

### The rules

- **It must be that person's own finger** (or the caretaker's). দাদু can
  reorder দাদু's doses and nothing else.
- **Only that person's doses are given up.** Choosing a late dose never
  silently drops the other person's medicine.
- **If the other person has a dose due first**, it is refused with
  **আগে অন্যজনের ঔষধ আছে** — that dose is genuinely still next, and only its
  owner may give it up. To reach a later dose, that person skips their **own**
  earlier one first.
- **Only a dose still ahead today.** One already over says **ইতিমধ্যেই শেষ**.
- **Never silent.** Every skip is written to the history and sent on Telegram.
- Everything clears at midnight. Tomorrow is untouched.

---

## Who can open what

| Who | Opens | Fingerprint IDs |
|---|---|---|
| **দাদু** | only দাদু's doses | 1–9 |
| **ঠাকুমা** | only ঠাকুমা's doses | 10–19 |
| **কেয়ারটেকার** | **any compartment, any dose** | 20–29 |

The caretaker's finger works anywhere a fingerprint is asked for. Nothing
special appears on screen — it simply opens — but it is recorded in the history
and sent on Telegram, because an override should never be invisible.

The caretaker takes no medicine and owns no compartment.

---

## Changing a fingerprint

যন্ত্র tab on the dashboard → **আঙুলের ছাপ**. Each person has a button:
**বদলান**, or **যুক্ত করুন** if they have none yet.

Press it, then **watch the box** — it tells you each step, and says it aloud.

1. **পুরানো ছাপ দিন** — the person puts their **current** finger on the sensor.
   The caretaker may do this for them.
2. **মধ্য ভাগের ছাপ দিন** — the new finger, flat and centred. Twice.
3. **বাম পাশের ছাপ দিন** — rolled slightly onto its left edge. Twice.
4. **ডান পাশের ছাপ দিন** — rolled slightly onto its right edge. Twice.
5. **নতুন ছাপ সংরক্ষিত** — done.

Six presses. **আবার একই ভাবে** means give the same angle again. The dashboard
shows a progress bar alongside.

Three angles because the sensor builds one template from two impressions and
rejects two that differ too much. Three stored templates is what lets a
slightly rolled finger still open the box — one flat template is exactly why a
fingerprint "stops working" after a few weeks.

### The rules

- **The old finger is required first**, so nobody with just the PIN can replace
  someone's fingerprint with their own.
- **Adding the first caretaker needs দাদু's or ঠাকুমা's finger.** A master key
  is the most privileged thing the box can be asked for.
- **The old print stops working**, but only once the new ones are safely
  stored. If enrolment fails halfway, the old finger still works.
- **A dose always wins.** A reminder starting mid-enrolment cancels it.

---

## The dashboard

Wi-Fi follows the power mode — there is nothing else to switch.

### Connecting

1. Make sure it is in **normal mode** (hold **BACK 3 s** if you see a clock)
2. On your phone: **Settings → Wi-Fi**
3. Join **`Oshudh-Box`**, password **`oshudh1234`**
4. The Bangla page opens by itself. If not, go to **`192.168.4.1`**

Your phone will warn "no internet". That is correct — the box makes its own
network. No router needed.

### The four tabs

| Tab | What |
|---|---|
| **সারসংক্ষেপ** | Next dose, and how many taken / missed / unconfirmed |
| **সময়সূচি** | Dose times, on/off, and **Bangla medicine names** |
| **ইতিহাস** | Every event the box has recorded |
| **যন্ত্র** | Health, clock, home Wi-Fi, fingerprints, Telegram, restart |

### The PIN

**`1234`** to begin with.

Looking is free. **Changing** anything asks for the PIN. Tap **পিন** at the top
right to enter it once.

**Change it before real use:** যন্ত্র → **পিন বদলান**. It asks for the current
PIN first, so nobody on the Wi-Fi can lock you out. The new PIN survives power
cuts.

### Dose times and medicine names

Both are set on the **সময়সূচি** tab, not with the buttons — four buttons cannot
type Bangla.

Two doses must be at least **15 minutes apart**, the same rule everywhere.

> **Medicine names must be in Bangla.** The box's font has no English letters
> at all, so `Napa` shows as empty boxes on the screen. The dashboard warns you
> as you type.

### Restarting without opening the box

যন্ত্র → **পুনরায় চালু**. Your phone drops off while it restarts — wait about
20 seconds and rejoin. You never need to reach the RST button inside.

### What the dashboard deliberately cannot do

- **It cannot open a compartment.** There is no such button and no such command
  in the box. Opening one needs a finger on the sensor, in person.
- **It cannot bypass the 15-minute rule.**

---

## Telegram alerts (optional)

Off until you switch it on. Everything else works the same without it.

### First: give the box internet

যন্ত্র → **হোম ওয়াই-ফাই** → your home network name and password →
**যুক্ত করুন**. Watch the **ইন্টারনেট** row: it turns **যুক্ত** within about ten
seconds. If it stays **নেই**, the name or password is wrong.

Your phone stays on `Oshudh-Box` throughout. The box is on both networks.

### Then: make the bot

Telegram does **not** let anything message a phone number. You make a free bot
once.

1. In Telegram, search **@BotFather** and send **`/newbot`**
2. It replies with a long token
3. যন্ত্র → **টেলিগ্রামে খবর** → tick **চালু করুন** → paste the token

### Then: add up to two people

**Each person sends the bot `/start` first**, just before you press their
button — a bot cannot message someone who has never contacted it.

1. Type their name (just a label for you)
2. Press **যুক্ত করুন** — the box finds their number itself
3. Press **সংরক্ষণ**

Add them **one at a time**: the button picks up whoever messaged most recently.

Then press **একটি পরীক্ষা মেসেজ পাঠান**. If it arrives, you are done.

### What gets sent

| When | Message |
|---|---|
| A dose is missed | ❌ who, which medicine, what time |
| The box opened but nobody confirmed | ⚠️ who, which medicine |
| Power was out across a dose | ❌ the dose that was lost |
| Opened with the override buttons | 🔓 who and when |
| The caretaker opens a compartment | 🔓 who the dose belonged to |
| A dose is skipped to move a later one forward | ⏭️ what was given up, what is next |
| A fingerprint is changed | 🔐 a notice |
| A course of medicine finishes | 🎉 who and which medicine |
| Three wrong fingerprints in five minutes | ⚠️ a warning |
| **Every evening at 9:00 pm** | 📋 **the whole day — every dose, taken or not** |

That evening summary is why there is no website to log into. The history comes
to you. Change the time with **দিনের হিসাব কখন পাঠাবে** — any hour and minute.

### It still works on battery

In power saving the Wi-Fi is off. When a dose is missed the box **turns the
radio on by itself for up to two minutes**, sends, and switches it off again.
If there is no internet then, the message waits and goes out when there is.

### Turning it off

Untick **চালু করুন** → **সংরক্ষণ**. Nothing else changes.

> The token is a password for your bot. Once saved the page shows only the
> first few characters — leave that field **blank** when changing other
> settings and the saved token is kept.

> Messages only ever go **out**. Nothing sent to the bot can control the box.
> There is no command anywhere that opens a compartment remotely.

---

## What the screens mean

| Screen | Meaning |
|---|---|
| **পরবর্তী** | Idle — next dose shown |
| **সাশ্রয়ী মোড** | Power saving — clock only |
| **ঔষধ গ্রহণের সময়** | Time to take medicine |
| **আঙুলের ছাপ দিন** | Put your finger on the sensor |
| **ভুল ব্যবহারকারী** | Wrong or unknown finger — nothing opened |
| **খোপ খুলেছে** | Compartment is open |
| **ঔষধ গ্রহণ সম্পন্ন** | Done, recorded |
| **ঔষধ গ্রহণ করা হয়নি** | Missed |
| **ঔষধ গ্রহণ নিশ্চিত হয়নি** | Opened but SELECT never pressed |
| **বিশেষ অনুমতি সক্রিয়** | Override was used |
| **ঔষধের কোর্স সম্পন্ন** | Course finished, slot switched off |
| **সময় সঠিক নয়** | Clock not set |
| **আজকের ঔষধ** | Browsing today's doses |
| **ইতিমধ্যেই শেষ** | That dose is already over |
| **এটিই পরবর্তী** | That dose is already the next one |
| **আগে অন্যজনের ঔষধ আছে** | The other person's dose comes first |
| **সময় ও শব্দ** | Clock, date and volume |
| **শব্দ ঠিক করুন** | Changing the volume |
| **ছাপ নিবন্ধন** | Enrolling a fingerprint |

---

## What it says out loud

Seventeen Bangla lines, all in the same voice. The full list is in
[audio/VOICE-SCRIPT.md](../audio/VOICE-SCRIPT.md).

Copy the whole of `audio/MP3/` to `/MP3/` on the SD card. At boot the Serial
Monitor reports what it found:

```
       Files detected: 17  (expected 17)
```

If files are missing it says so. Nothing breaks — the screen carries the same
instruction, and **the buzzer sounds for every reminder even with no card at
all.** A reminder is never silent.

---

## Serial commands

| Key | Does |
|---|---|
| `P` | Show the 4 slots |
| `N` | Show next dose (also refreshes the screen) |
| `L` | Show the event log |
| `E` | Redraw the screen |
| `S` | Power saving on/off |
| `C` | Diagnose the audio module and SD card |
| `K` | **Test a lock** — then press 1–4. Fires that compartment once |
| `1`–`9`, `0` | Play a voice track |
| `F` | Switch SD layout (root ↔ `/MP3/`) |
| `W` | Erase the event log (asks first) |
| `R` / `D` | Reload settings / restore defaults |

---

## When something looks wrong

| Symptom | Fix |
|---|---|
| Can't find `Oshudh-Box` | It is in power saving. Hold **BACK 3 s** |
| Dashboard page won't load | Go to `192.168.4.1`. Check your phone joined `Oshudh-Box` |
| Dashboard stopped responding | যন্ত্র → **পুনরায় চালু**, wait 20 s, rejoin |
| **ভুল পিন** | PIN is `1234` unless you changed it |
| Forgot the PIN | Serial `D` restores defaults, including PIN `1234` |
| Screen stuck on an old picture | Press **`E`** — it now hard-resets the panel, not just repaints |
| Screen shows torn dots or half-drawn rubbish | Press **`E`**. If it stays, check the **BUSY** wire on GPIO15 — the boot log reports what it reads |
| A black box in the Bangla text | Tell me which screen — a font character is missing |
| A compartment won't open on a correct finger | Press **`K`** then that number. If it clicks, the lock is fine and the problem is in the dose; if not, it is wiring or the 12 V supply |
| **শব্দ যন্ত্র: কার্ড পড়া যাচ্ছে না** | The card. Reseat it; FAT32, 32 GB or smaller |
| **শব্দ যন্ত্র: যন্ত্র সাড়া দেয় না** | Wiring or 5 V power, not the card |
| No voice, or the wrong sentence plays | Tracks missing from the card. Check the boot count, copy `audio/MP3/` |
| **ঘড়ি: সমস্যা** | Check the clock wiring; `WARNING: RTC lost power` means a new coin cell |
| Serial says "not connected" | Only on battery. Hold **BACK 3 s**, or press RST |

> **If a compartment ever clicks open at power-up, stop and say so.** That is a
> fault, not a quirk. It should stay shut from the instant the box starts.
