#pragma once

#include <cstdint>

#include "config.h"
#include "ui/ui_scale.h"

namespace ui {

/**
 * Drawing front end for the screens laid out on a fixed 480x320 grid (forecast,
 * water, clock). Coordinates are given in that design grid and mapped to the panel:
 * scaled by ui::S() and centred horizontally (on the 800x480 panel the 720x480
 * design area gets 40 px on either side). Edges are mapped one by one, so shapes that
 * touch in the design still touch on the panel. Text uses the board's 1.5x fonts;
 * textWidth() / fontHeight() report design-grid sizes so layout code keeps working.
 *
 * G is a LovyanGFX target (LGFX_Sprite or the panel device).
 */
template <typename G>
class ScaledGfx {
 public:
  static constexpr int kDesignW = 480;
  static constexpr int kDesignH = 320;
  static constexpr int kOffX = (config::kDisplayWidth - S(kDesignW)) / 2;
  static constexpr int kOffY = (config::kDisplayHeight - S(kDesignH)) / 2;

  explicit ScaledGfx(G* g = nullptr) : g_(g) {}
  void bind(G* g) { g_ = g; }
  G* raw() const { return g_; }
  explicit operator bool() const { return g_ != nullptr; }

  // --- coordinate mapping ---
  static constexpr int X(int x) { return kOffX + S(x); }
  static constexpr int Y(int y) { return kOffY + S(y); }
  /** Panel point -> design grid (for touch). */
  static int toDesignX(int px) { return ((px - kOffX) * kScaleDen * 2 + kScaleNum) / (kScaleNum * 2); }
  static int toDesignY(int py) { return ((py - kOffY) * kScaleDen * 2 + kScaleNum) / (kScaleNum * 2); }

  // --- shapes ---
  void fillScreen(uint32_t c) { g_->fillScreen(c); }
  void fillRect(int x, int y, int w, int h, uint32_t c) {
    g_->fillRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), c);
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) {
    g_->fillRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), S(r), c);
  }
  void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) {
    g_->drawRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), S(r), c);
  }
  void drawFastHLine(int x, int y, int w, uint32_t c) {
    g_->fillRect(X(x), Y(y), X(x + w) - X(x), line(), c);
  }
  void drawFastVLine(int x, int y, int h, uint32_t c) {
    g_->fillRect(X(x), Y(y), line(), Y(y + h) - Y(y), c);
  }
  void drawLine(int x0, int y0, int x1, int y1, uint32_t c) {
    g_->drawLine(X(x0), Y(y0), X(x1), Y(y1), c);
    if (line() > 1) g_->drawLine(X(x0) + 1, Y(y0), X(x1) + 1, Y(y1), c);
  }
  void drawPixel(int x, int y, uint32_t c) { g_->fillRect(X(x), Y(y), line(), line(), c); }
  void fillCircle(int x, int y, int r, uint32_t c) { g_->fillCircle(X(x), Y(y), S(r), c); }
  void drawCircle(int x, int y, int r, uint32_t c) {
    g_->drawCircle(X(x), Y(y), S(r), c);
    if (line() > 1) g_->drawCircle(X(x), Y(y), S(r) - 1, c);
  }
  void fillEllipse(int x, int y, int rx, int ry, uint32_t c) {
    g_->fillEllipse(X(x), Y(y), S(rx), S(ry), c);
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    g_->fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), c);
  }
  void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) {
    g_->fillArc(X(x), Y(y), S(r0), S(r1), a0, a1, c);
  }
  void setClipRect(int x, int y, int w, int h) {
    g_->setClipRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y));
  }
  void clearClipRect() { g_->clearClipRect(); }

  // --- text ---
  template <typename F>
  void setFont(const F* f) { g_->setFont(f); }
  void setTextSize(float s) { g_->setTextSize(s); }
  void setTextDatum(textdatum_t d) { g_->setTextDatum(d); }
  template <typename C>
  void setTextColor(C c) { g_->setTextColor(c); }
  template <typename C>
  void setTextColor(C fg, C bg) { g_->setTextColor(fg, bg); }
  void setTextPadding(int w) { g_->setTextPadding(w > 0 ? S(w) : 0); }
  size_t drawString(const char* s, int x, int y) { return g_->drawString(s, X(x), Y(y)); }
  /** Width in design pixels. */
  int textWidth(const char* s) { return toDesign(static_cast<int>(g_->textWidth(s))); }
  int fontHeight() { return toDesign(static_cast<int>(g_->fontHeight())); }

  // --- pass-through ---
  template <typename... A>
  void setPaletteColor(A... a) { g_->setPaletteColor(a...); }
  template <typename... A>
  uint16_t color565(A... a) { return g_->color565(a...); }
  void pushSprite(int x, int y) { g_->pushSprite(x, y); }

 private:
  static constexpr int line() { return kScaleNum > kScaleDen ? 2 : 1; }  // 1 design px
  static int toDesign(int px) { return (px * kScaleDen * 2 + kScaleNum) / (kScaleNum * 2); }
  G* g_;
};

}  // namespace ui
