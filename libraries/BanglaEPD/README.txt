BanglaEPD v0.4 — Step 3E

WHAT CHANGED
------------
- Variable-length shaped clusters (up to 8 Unicode codepoints).
- Longest-cluster matching at runtime.
- Supports generated conjuncts instead of only 2/3-codepoint clusters.

KEEP
----
Documents\Arduino\libraries\BanglaUTF8

REPLACE
-------
Replace the old:
Documents\Arduino\libraries\BanglaEPD

with this v0.4 BanglaEPD folder.

IMPORTANT
---------
The included BanglaFont.cpp is only a placeholder.
Generate a real BanglaFont.cpp with the Step 3E HarfBuzz generator,
then replace the placeholder.

STEP 3E TARGET
--------------
- Common Bengali conjuncts
- রেফ
- র-ফলা
- য-ফলা
- Common conjunct + vowel-sign combinations
- Existing কার/diacritic support from Step 3D

This is a practical embedded Bengali renderer, not a complete runtime
implementation of HarfBuzz on the ESP32. The generator pre-shapes the
clusters and the ESP32 renders them directly.
