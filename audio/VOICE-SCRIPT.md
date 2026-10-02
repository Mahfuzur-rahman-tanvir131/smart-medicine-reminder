# Bangla voice files

Made at **https://www.narakeet.com/languages/bangla-text-to-speech/**
Voice: **Mahiya (Female)** · Format: **MP3** · No account needed.

Files go on the SD card as `/MP3/0001.mp3` … `/MP3/0017.mp3`.
The number in the filename is what the firmware asks for — nothing else about
the file matters.

---

## The original ten

| File | Bangla |
|---|---|
| 0001 | দাদু, ঔষধ গ্রহণের সময় হয়েছে। |
| 0002 | ঠাকুমা, ঔষধ গ্রহণের সময় হয়েছে। |
| 0003 | আঙুলের ছাপ দিন। |
| 0004 | ঔষধ গ্রহণ সম্পন্ন হয়েছে। |
| 0005 | ভুল ব্যবহারকারী। আবার আঙুলের ছাপ দিন। |
| 0006 | ঔষধের কোর্স সম্পন্ন হয়েছে। |
| 0007 | ঔষধ গ্রহণ করা হয়নি। |
| 0008 | ঔষধ গ্রহণ নিশ্চিত হয়নি। |
| 0009 | দরজা বন্ধ করুন। |
| 0010 | এটি এই ঔষধের শেষ গ্রহণ ছিল। |

## The seven new ones

Six are for changing a fingerprint, one for moving a dose forward.

| File | Bangla | Played when |
|---|---|---|
| 0011 | আঙুলের ছাপ বদলানো হবে। পুরানো ছাপ দিন। | enrolment starts, asking for the current finger |
| 0012 | মধ্য ভাগের ছাপ দিন। | first angle |
| 0013 | বাম পাশের ছাপ দিন। | second angle |
| 0014 | ডান পাশের ছাপ দিন। | third angle |
| 0015 | আবার একই ভাবে দিন। | the second impression of each angle |
| 0016 | নতুন ছাপ সংরক্ষিত হয়েছে। | enrolment finished |
| 0017 | পরবর্তী ঔষধ নির্বাচিত হয়েছে। | a dose was moved forward |

---

## Making them by hand

1. Open the link above.
2. Paste **one** line into the text box.
3. Voice **Mahiya (Female)**, format **MP3**.
4. **Create audio** → wait a few seconds → **Download**.
5. Rename to `0011.mp3` … `0017.mp3` and copy to `/MP3/` on the card.

The free tier allows 20 generated files before it asks you to sign up, which is
more than enough for these seven.

---

## If the files are missing

Nothing breaks. `safePlayTrack()` asks the module for a track that is not
there, the module ignores it, and the e-paper carries the same instruction on
its own. The box is fully usable without these — they are an improvement for
দাদু and ঠাকুমা, not a dependency.

The same is true of the original ten: the buzzer still sounds for every
reminder even with no SD card at all.
