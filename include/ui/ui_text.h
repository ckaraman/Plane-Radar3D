#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/paint.h"

/**
 * Smooth text for the control screen: Inter, anti-aliased (4-bit coverage), blended into
 * a paint::Canvas with the same dithering as the rest of the screen. UTF-8 strings;
 * letters a font lacks are left out. Fonts: tools/fonts/make_ui_fonts.py (5" and 5B).
 */
namespace ui::text {

struct Glyph {
  uint32_t cp;    // code point
  uint32_t off;   // into the font's bits (two pixels per byte, high nibble first)
  uint8_t w, h;
  int8_t x_off;   // from the pen position
  int8_t y_off;   // top of the bitmap, from the baseline (negative = above)
  uint8_t adv;
};

struct Font {
  const uint8_t* bits;
  const Glyph* glyphs;  // sorted by code point
  uint16_t count;
  uint8_t ascent, descent, line;
};

#if defined(BOARD_WS5)
extern const Font Ui13;   // Inter Medium: small labels, times, hints
extern const Font Ui13b;  // Inter SemiBold: page switch, RADAR, KONTROL
extern const Font Ui15b;
extern const Font Ui16;   // Inter Regular: artist
extern const Font Ui18b;
extern const Font Ui22b;  // title
#endif

enum class Align { Left, Center, Right };

/** Width in pixels (tracking: extra pixels between letters). */
int width(const Font& f, const char* utf8, int tracking = 0);

/** Text with its baseline at y. Returns the width drawn. */
int draw(const paint::Canvas& c, const Font& f, const char* utf8, int x, int baseline, paint::Rgb col,
         int alpha = 255, Align align = Align::Left, int tracking = 0);

/** Text centred vertically on cy (by its capital height). */
int drawMid(const paint::Canvas& c, const Font& f, const char* utf8, int x, int cy, paint::Rgb col,
            int alpha = 255, Align align = Align::Left, int tracking = 0);

/** in, cut to max_w with "…" when longer. */
void fit(const Font& f, const char* in, int max_w, char* out, size_t cap);

}  // namespace ui::text
