#include "ui/ui_text.h"

#include <cstring>

namespace ui::text {

namespace {

/** Next code point of a UTF-8 string (advances s); 0 at the end. */
uint32_t nextCp(const char*& s) {
  const auto* p = reinterpret_cast<const uint8_t*>(s);
  if (*p == 0) return 0;
  uint32_t cp;
  int extra;
  if (*p < 0x80) {
    cp = *p, extra = 0;
  } else if ((*p & 0xE0) == 0xC0) {
    cp = *p & 0x1F, extra = 1;
  } else if ((*p & 0xF0) == 0xE0) {
    cp = *p & 0x0F, extra = 2;
  } else if ((*p & 0xF8) == 0xF0) {
    cp = *p & 0x07, extra = 3;
  } else {
    ++s;
    return 0xFFFD;
  }
  ++p;
  for (int i = 0; i < extra; ++i) {
    if ((*p & 0xC0) != 0x80) {
      s = reinterpret_cast<const char*>(p);
      return 0xFFFD;
    }
    cp = (cp << 6) | (*p & 0x3F);
    ++p;
  }
  s = reinterpret_cast<const char*>(p);
  return cp;
}

const Glyph* find(const Font& f, uint32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].cp < cp) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return nullptr;
}

/** A letter the font lacks: its plain form for a few, else nothing. */
const Glyph* glyphFor(const Font& f, uint32_t cp) {
  if (const Glyph* g = find(f, cp)) return g;
  if (cp == 0x2032 || cp == 0x2035) return find(f, '\'');
  if (cp == 0x00A0 || cp == 0x2009 || cp == 0x202F) return find(f, ' ');
  return nullptr;
}

void blend(const paint::Canvas& c, int x, int y, paint::Rgb col, int a) {
  if (x < 0 || y < 0 || x >= c.w || y >= c.h || a <= 0) return;
  uint16_t& p = c.px[y * c.w + x];
  if (a >= 255) {
    p = paint::pack(col.r, col.g, col.b, x, y);
    return;
  }
  int r, g, b;
  paint::unpack(p, &r, &g, &b);
  p = paint::pack(r + ((col.r - r) * a) / 255, g + ((col.g - g) * a) / 255, b + ((col.b - b) * a) / 255, x, y);
}

}  // namespace

int width(const Font& f, const char* s, int tracking) {
  int w = 0, n = 0;
  while (uint32_t cp = nextCp(s)) {
    if (const Glyph* g = glyphFor(f, cp)) {
      w += g->adv + tracking;
      ++n;
    }
  }
  return n ? w - tracking : 0;
}

int draw(const paint::Canvas& c, const Font& f, const char* s, int x, int baseline, paint::Rgb col, int alpha,
         Align align, int tracking) {
  const int w = width(f, s, tracking);
  if (align == Align::Center) x -= w / 2;
  if (align == Align::Right) x -= w;
  int pen = x;
  while (uint32_t cp = nextCp(s)) {
    const Glyph* g = glyphFor(f, cp);
    if (g == nullptr) continue;
    const uint8_t* bits = f.bits + g->off;
    const int gx = pen + g->x_off, gy = baseline + g->y_off;
    for (int r = 0; r < g->h; ++r) {
      for (int q = 0; q < g->w; ++q) {
        const int i = r * g->w + q;
        const int v = (i & 1) ? (bits[i >> 1] & 0x0F) : (bits[i >> 1] >> 4);
        if (v) blend(c, gx + q, gy + r, col, (v * 17 * alpha) / 255);
      }
    }
    pen += g->adv + tracking;
  }
  return w;
}

int drawMid(const paint::Canvas& c, const Font& f, const char* s, int x, int cy, paint::Rgb col, int alpha,
            Align align, int tracking) {
  const Glyph* h = find(f, 'H');
  const int cap = h ? -h->y_off : f.ascent * 7 / 10;
  return draw(c, f, s, x, cy + cap / 2, col, alpha, align, tracking);
}

void fit(const Font& f, const char* in, int max_w, char* out, size_t cap) {
  strlcpy(out, in, cap);
  if (width(f, out) <= max_w) return;
  static const char kDots[] = "\xE2\x80\xA6";  // …
  const int dots = width(f, kDots);
  size_t n = strlen(out);
  while (n > 0) {
    do {
      --n;
    } while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xC0) == 0x80);
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    if (width(f, out) + dots <= max_w && n + sizeof(kDots) <= cap) {
      strlcat(out, kDots, cap);
      return;
    }
  }
}

}  // namespace ui::text
