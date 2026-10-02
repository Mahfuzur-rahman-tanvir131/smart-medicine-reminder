#include "BanglaUTF8.h"

uint8_t utf8ToCodepoint(const char* s, uint32_t &codepoint) {
  uint8_t c = (uint8_t)s[0];

  // 1-byte ASCII
  if (c < 0x80) {
    codepoint = c;
    return 1;
  }

  // 2-byte UTF-8
  if ((c & 0xE0) == 0xC0) {
    codepoint = ((uint32_t)(c & 0x1F) << 6) |
                ((uint32_t)(s[1] & 0x3F));
    return 2;
  }

  // 3-byte UTF-8
  if ((c & 0xF0) == 0xE0) {
    codepoint = ((uint32_t)(c & 0x0F) << 12) |
                ((uint32_t)(s[1] & 0x3F) << 6) |
                ((uint32_t)(s[2] & 0x3F));
    return 3;
  }

  // 4-byte UTF-8
  if ((c & 0xF8) == 0xF0) {
    codepoint = ((uint32_t)(c & 0x07) << 18) |
                ((uint32_t)(s[1] & 0x3F) << 12) |
                ((uint32_t)(s[2] & 0x3F) << 6) |
                ((uint32_t)(s[3] & 0x3F));
    return 4;
  }

  // Invalid UTF-8
  codepoint = '?';
  return 1;
}

bool isBanglaCodepoint(uint32_t cp) {
  return (cp >= 0x0980 && cp <= 0x09FF);
}

void printCodepoints(const char* s) {
  while (*s) {
    uint32_t cp;
    uint8_t used = utf8ToCodepoint(s, cp);

    Serial.print("U+");
    if (cp < 0x1000) Serial.print("0");
    if (cp < 0x100)  Serial.print("0");
    if (cp < 0x10)   Serial.print("0");
    Serial.println(cp, HEX);

    s += used;
  }
}
