#ifndef BANGLA_EPD_H
#define BANGLA_EPD_H

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <BanglaUTF8.h>
#include "BanglaFont.h"

template <typename DisplayT>
class BanglaEPD {
public:
  explicit BanglaEPD(DisplayT& display)
    : _display(display),
      _x(0),
      _baselineY(0),
      _lineStartX(0),
      _lineHeight(34) {}

  void setCursor(int16_t x, int16_t baselineY) {
    _x = x;
    _lineStartX = x;
    _baselineY = baselineY;
  }

  void setLineHeight(uint8_t px) {
    _lineHeight = px;
  }

  int16_t getCursorX() const {
    return _x;
  }

  void print(const char* text) {
    const char* p = text;

    while (*p) {
      uint32_t firstCp = 0;
      uint8_t firstBytes = utf8ToCodepoint(p, firstCp);

      if (firstCp == '\n') {
        _x = _lineStartX;
        _baselineY += _lineHeight;
        p += firstBytes;
        continue;
      }

      if (firstCp == 0x0020) {
        _x += 10;
        p += firstBytes;
        continue;
      }

      uint32_t cps[BANGLA_MAX_CLUSTER_CP] = {0};
      uint8_t byteLens[BANGLA_MAX_CLUSTER_CP] = {0};
      uint8_t count = 0;

      const char* q = p;

      while (*q && count < BANGLA_MAX_CLUSTER_CP) {
        uint32_t cp = 0;
        uint8_t used = utf8ToCodepoint(q, cp);

        // Never let a cluster cross whitespace/newline.
        if (count > 0 && (cp == 0x0020 || cp == '\n')) {
          break;
        }

        cps[count] = cp;
        byteLens[count] = used;
        count++;
        q += used;
      }

      bool matched = false;

      // Longest generated shaped cluster wins.
      for (int len = count; len >= 2; --len) {
        const BanglaClusterGlyph* cluster =
          getBanglaClusterGlyph(cps, static_cast<uint8_t>(len));

        if (cluster != nullptr) {
          drawCluster(cluster);
          _x += cluster->xAdvance;

          for (int i = 0; i < len; ++i) {
            p += byteLens[i];
          }

          matched = true;
          break;
        }
      }

      if (matched) {
        continue;
      }

      // Fall back to one independently renderable codepoint.
      const BanglaGlyph* glyph = getBanglaGlyph(firstCp);

      if (glyph != nullptr) {
        drawGlyph(glyph);
        _x += glyph->xAdvance;
      } else {
        drawMissingGlyph();
        _x += 18;
      }

      p += firstBytes;
    }
  }

private:
  DisplayT& _display;
  int16_t _x;
  int16_t _baselineY;
  int16_t _lineStartX;
  uint8_t _lineHeight;

  void drawGlyph(const BanglaGlyph* glyph) {
    _display.drawBitmap(
      _x + glyph->xOffset,
      _baselineY + glyph->yOffset,
      glyph->bitmap,
      glyph->width,
      glyph->height,
      GxEPD_BLACK
    );
  }

  void drawCluster(const BanglaClusterGlyph* glyph) {
    _display.drawBitmap(
      _x + glyph->xOffset,
      _baselineY + glyph->yOffset,
      glyph->bitmap,
      glyph->width,
      glyph->height,
      GxEPD_BLACK
    );
  }

  void drawMissingGlyph() {
    const int16_t y = _baselineY - 22;
    _display.drawRect(_x, y, 14, 20, GxEPD_BLACK);
    _display.drawLine(_x, y, _x + 13, y + 19, GxEPD_BLACK);
    _display.drawLine(_x + 13, y, _x, y + 19, GxEPD_BLACK);
  }
};

#endif
