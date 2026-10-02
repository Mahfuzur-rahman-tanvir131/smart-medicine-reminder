#ifndef BANGLA_FONT_H
#define BANGLA_FONT_H

#include <Arduino.h>

static const uint8_t BANGLA_MAX_CLUSTER_CP = 8;

struct BanglaGlyph {
  uint32_t codepoint;
  const uint8_t* bitmap;
  uint8_t width;
  uint8_t height;
  int16_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
};

struct BanglaClusterGlyph {
  uint32_t codepoints[BANGLA_MAX_CLUSTER_CP];
  uint8_t length;
  const uint8_t* bitmap;
  uint8_t width;
  uint8_t height;
  int16_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
};

const BanglaGlyph* getBanglaGlyph(uint32_t codepoint);

const BanglaClusterGlyph* getBanglaClusterGlyph(
  const uint32_t* codepoints,
  uint8_t length
);

#endif
