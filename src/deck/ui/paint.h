#pragma once

#include <cstdint>

/**
 * Soft drawing for the stream deck screens on the 5" panel: smooth gradients and
 * see-through, anti-aliased shapes on a 16-bit canvas. The panel shows 65 k colours
 * (RGB565), where a plain gradient falls into visible steps, so every colour that is
 * computed here is written with ordered dithering (4 x 4 Bayer) and the steps vanish.
 *
 * Canvas pixels are byte-swapped RGB565, the format of LovyanGFX 16-bit sprites and of
 * the panel's frame buffers, so a canvas can be drawn on with the sprite's own text and
 * shape functions too.
 */
namespace ui::paint {

struct Rgb {
  uint8_t r, g, b;
};

/** A canvas: w x h pixels, row by row. */
struct Canvas {
  uint16_t* px;
  int w;
  int h;
};

/** Dithered pack of an 8-bit colour at (x, y) to a canvas pixel. */
uint16_t pack(int r, int g, int b, int x, int y);
/** Canvas pixel to 8-bit components. */
void unpack(uint16_t p, int* r, int* g, int* b);

/** A smooth darkening over rows y0..y1, alpha a0 -> a1 (0..255, eased). Applied while a
 *  colour is still exact (before it is packed): shading packed pixels would bring the
 *  steps back. */
struct Shade {
  int y0, y1;
  Rgb col;
  int a0, a1;
};
/** Shade alpha (0..1) of row y. */
float shadeAlpha(const Shade& s, int y);
/** 8-bit colour with the shades applied, packed (dithered) at (x, y). */
uint16_t packShaded(float r, float g, float b, int x, int y, const Shade* shades, int n);

/** The deck background: indigo to plum diagonal with a lavender glow top right and a
 *  peach glow bottom left (the approved design), dithered. Fills the whole canvas;
 *  optional shades are applied on the way. */
void background(const Canvas& c, const Shade* shades = nullptr, int n = 0);
/** The background's exact colour at (x, y) of a w x h canvas (to blend other things into). */
void backgroundRgb(int w, int h, int x, int y, float* r, float* g, float* b);

/** Rounded rectangle in colour `c` at `alpha` (0..255) over what is there, with
 *  anti-aliased edges. `border_alpha` > 0 adds a 1 px outline in `border`. */
void roundRect(const Canvas& c, int x, int y, int w, int h, int radius, Rgb fill, int alpha,
               Rgb border = {255, 255, 255}, int border_alpha = 0);

/** Anti-aliased disc. */
void circle(const Canvas& c, float cx, float cy, float r, Rgb col, int alpha);

/** Anti-aliased ring sector from a0 to a1 degrees (0 = 12 o'clock, clockwise). */
void arc(const Canvas& c, float cx, float cy, float r_out, float r_in, float a0, float a1, Rgb col,
         int alpha);

/** Horizontal bar in a three-colour gradient (left to right), rounded ends. */
void gradientBar(const Canvas& c, int x, int y, int w, int h, Rgb c0, Rgb c1, Rgb c2);

/** Darkens rows y0..y1 towards `col`, alpha running smoothly from a0 to a1 (0..255). */
void shade(const Canvas& c, int y0, int y1, Rgb col, int a0, int a1);

/** Copy of a canvas region (same size canvases). */
void copyRect(const Canvas& dst, const Canvas& src, int x, int y, int w, int h);

/** Three-stop colour ramp, t = 0..1. */
Rgb ramp(Rgb c0, Rgb c1, Rgb c2, float t);

}  // namespace ui::paint
