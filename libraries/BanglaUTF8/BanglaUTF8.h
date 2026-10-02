#ifndef BANGLA_UTF8_H
#define BANGLA_UTF8_H

#include <Arduino.h>

uint8_t utf8ToCodepoint(const char* s, uint32_t &codepoint);
void printCodepoints(const char* s);
bool isBanglaCodepoint(uint32_t cp);

#endif
