#include "ui/deck_screen.h"

#if defined(BOARD_WS5)

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "hardware/console.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/deck_store.h"
#include "services/hid_keys.h"
#include "ui/fonts_tr.h"
#include "ui/screen_switch.h"
#include "ui/ui_scale.h"

namespace ui::deck {

namespace {

using services::hid::Media;
namespace store = services::deck;

constexpr unsigned long kPressFlashMs = 140;
/** Knob: a full turn goes from silent to full volume, so one volume key per 360 / steps
 *  degrees (macOS: 16 steps, Windows: 50; set on the setup page). */
float stepDeg() { return 360.0f / static_cast<float>(services::deck::volumeSteps()); }
/** The estimated level is saved this long after the last change (flash wear). */
constexpr unsigned long kSaveLevelMs = 3000;
/** A touch on the knob that turns less than this, short and still, is a tap (mute). */
constexpr float kTapMaxDeg = 8.0f;
constexpr unsigned long kTapMaxMs = 700;

// --- Layout: title bar, 4 x 3 keys on the left, the knob on the right, RADAR bottom right.
constexpr int kW = config::kDisplayWidth;
constexpr int kTopH = S(34);
constexpr int kGap = S(8);
constexpr int kMargin = S(8);
constexpr int kAreaY = kTopH + S(4);
constexpr int kAreaH = ui::kSwitchY - S(6) - kAreaY;
constexpr int kKnobD = S(200) < kAreaH ? S(200) : kAreaH;
constexpr int kKnobX = kW - kMargin - kKnobD;
constexpr int kKnobY = kAreaY + (kAreaH - kKnobD) / 2;
constexpr int kKnobR = kKnobD / 2 - S(2);
constexpr int kCols = 4;
constexpr int kRows = 3;
constexpr int kKeysW = kKnobX - S(12) - kMargin;
constexpr int kKeyW = (kKeysW - (kCols - 1) * kGap) / kCols;
constexpr int kKeyH = (kAreaH - (kRows - 1) * kGap) / kRows;
static_assert(kCols * kRows == store::kSlots, "one key per stored slot");

uint16_t cBg() { return tft.color565(6, 10, 18); }
uint16_t cKey() { return tft.color565(18, 30, 46); }
uint16_t cKeyDown() { return tft.color565(60, 150, 210); }
uint16_t cEdge() { return tft.color565(48, 86, 110); }
uint16_t cDim() { return tft.color565(125, 139, 150); }
uint16_t cCyan() { return tft.color565(110, 210, 255); }
uint16_t cRed() { return tft.color565(235, 90, 80); }

LGFX_Sprite s_knob(&tft);
bool s_knob_ok = false;
// Volume as the deck thinks it is: the USB keyboard cannot read the PC's level, so the
// deck counts its own steps. Turning past either end keeps sending keys, which brings the
// count back in line after the volume was changed elsewhere (the PC stops at 0 / max).
int s_level = -1;  // 0..volumeSteps(); -1 = not loaded yet
bool s_muted = false;
unsigned long s_level_changed_ms = 0;
bool s_level_dirty = false;

void loadLevel() {
  Preferences p;
  int v = services::deck::volumeSteps() / 2;
  if (p.begin("deck", true)) {
    v = p.getInt("vol", v);
    p.end();
  }
  s_level = constrain(v, 0, services::deck::volumeSteps());
}

void saveLevel() {
  Preferences p;
  if (p.begin("deck", false)) {
    p.putInt("vol", s_level);
    p.end();
  }
  s_level_dirty = false;
}

int s_pressed = -1;  // key drawn pressed
unsigned long s_pressed_ms = 0;
bool s_flash = false;  // released, highlight still showing
int s_usb_shown = -1;
uint32_t s_gen_shown = 0;
uint16_t* s_icon_buf = nullptr;  // one icon composed onto the key colour

// Touch state.
enum class Target { None, Key, Knob, Back };
Target s_target = Target::None;
bool s_down = false;
int s_key = -1;
int s_down_x = 0, s_down_y = 0;
int s_last_x = 0, s_last_y = 0;  // last finger position (none on release)
unsigned long s_down_ms = 0;
float s_last_deg = 0.0f, s_accum_deg = 0.0f, s_total_deg = 0.0f;
int s_max_move = 0;

void keyRect(int i, int* x, int* y) {
  *x = kMargin + (i % kCols) * (kKeyW + kGap);
  *y = kAreaY + (i / kCols) * (kKeyH + kGap);
}

int keyAt(int x, int y) {
  for (int i = 0; i < store::kSlots; ++i) {
    int kx, ky;
    keyRect(i, &kx, &ky);
    if (x >= kx && x < kx + kKeyW && y >= ky && y < ky + kKeyH) return i;
  }
  return -1;
}

constexpr int kKnobCx = kKnobX + kKnobD / 2;
constexpr int kKnobCy = kKnobY + kKnobD / 2;

bool inKnob(int x, int y) {
  const int dx = x - kKnobCx, dy = y - kKnobCy;
  return dx * dx + dy * dy <= (kKnobR + S(10)) * (kKnobR + S(10));
}

// --- Drawing -----------------------------------------------------------------------

template <typename Gfx>
void speakerIcon(Gfx& g, int cx, int cy, int s, uint16_t c) {
  g.fillRect(cx - s, cy - s / 2, s * 2 / 3, s, c);
  g.fillTriangle(cx - s / 3, cy - s / 2, cx + s / 3, cy - s, cx + s / 3, cy + s, c);
  g.fillRect(cx - s / 3, cy - s / 2, s * 2 / 3 + 1, s, c);
}

/** Built-in pictures for media keys without one of their own. */
void mediaIcon(Media m, int cx, int cy, uint16_t c) {
  const int s = S(14);
  switch (m) {
    case Media::Previous:
      tft.fillRect(cx - s, cy - s, S(3), 2 * s, c);
      tft.fillTriangle(cx - s + S(3), cy, cx, cy - s, cx, cy + s, c);
      tft.fillTriangle(cx, cy, cx + s, cy - s, cx + s, cy + s, c);
      break;
    case Media::Next:
      tft.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx, cy, c);
      tft.fillTriangle(cx, cy - s, cx, cy + s, cx + s - S(3), cy, c);
      tft.fillRect(cx + s - S(3), cy - s, S(3), 2 * s, c);
      break;
    case Media::PlayPause:
      tft.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + S(2), cy, c);
      tft.fillRect(cx + S(5), cy - s, S(4), 2 * s, c);
      tft.fillRect(cx + S(12), cy - s, S(4), 2 * s, c);
      break;
    case Media::VolumeUp:
    case Media::VolumeDown:
    case Media::Mute: {
      speakerIcon(tft, cx - S(6), cy, s, c);
      const int mx = cx + S(12);
      if (m == Media::Mute) {
        for (int t = -1; t <= 1; ++t) {
          tft.drawLine(mx - S(5) + t, cy - S(5), mx + S(5) + t, cy + S(5), c);
          tft.drawLine(mx - S(5) + t, cy + S(5), mx + S(5) + t, cy - S(5), c);
        }
      } else {
        tft.fillRect(mx - S(6), cy - S(1), S(12), S(3), c);
        if (m == Media::VolumeUp) tft.fillRect(mx - S(1), cy - S(6), S(3), S(12), c);
      }
      break;
    }
  }
}

void drawKey(int i, bool down) {
  int x, y;
  keyRect(i, &x, &y);
  const store::Slot& k = store::slot(i);
  const uint16_t fill = down ? cKeyDown() : cKey();
  tft.fillRoundRect(x, y, kKeyW, kKeyH, S(10), fill);
  tft.drawRoundRect(x, y, kKeyW, kKeyH, S(10), down ? 0xFFFF : cEdge());
  if (k.kind == store::Kind::Empty && !k.label[0] && !store::icon(i)) return;
  const int cx = x + kKeyW / 2;
  const uint16_t* pic = store::icon(i);
  const int px = store::iconPx();
  const bool has_label = k.label[0] != '\0';
  tft.setTextColor(0xFFFF, fill);
  tft.setTextDatum(textdatum_t::middle_center);
  if (pic != nullptr && s_icon_buf != nullptr) {
    // Picture: magenta pixels take the key colour, then one push.
    for (int p = 0; p < px * px; ++p) s_icon_buf[p] = pic[p] == store::kTransparent ? fill : pic[p];
    const int iy = has_label ? y + S(5) : y + (kKeyH - px) / 2;
    tft.pushImage(cx - px / 2, iy, px, px, reinterpret_cast<const lgfx::rgb565_t*>(s_icon_buf));
    if (has_label) {
      displayFontSetBitmap(tft, &ui::font::Sans8);
      tft.drawString(k.label, cx, y + kKeyH - S(10));
    }
  } else if (k.kind == store::Kind::Media) {
    mediaIcon(static_cast<Media>(k.code), cx, y + kKeyH / 2 - S(9), down ? 0xFFFF : cCyan());
    displayFontSetBitmap(tft, &ui::font::Sans8);
    tft.drawString(k.label, cx, y + kKeyH - S(12));
  } else {
    // Text only: the largest font that fits the key.
    const lgfx::GFXfont* fonts[] = {&ui::font::SansBold18, &ui::font::SansBold12, &ui::font::SansBold9,
                                    &ui::font::Sans8};
    for (const auto* f : fonts) {
      displayFontSetBitmap(tft, f);
      if (tft.textWidth(k.label) <= kKeyW - S(10)) break;
    }
    tft.drawString(k.label, cx, y + kKeyH / 2);
  }
  tft.setTextDatum(textdatum_t::top_left);
}

void drawKnob() {
  if (!s_knob_ok) return;
  LGFX_Sprite& g = s_knob;
  const int c = kKnobD / 2, R = kKnobR;
  g.fillScreen(cBg());
  // Ring: the level as an arc from the top, clockwise; a full ring = full volume.
  const int steps = services::deck::volumeSteps();
  const int level = s_level < 0 ? 0 : (s_level > steps ? steps : s_level);
  const int rw = S(14);  // ring width
  g.fillCircle(c, c, R, tft.color565(28, 48, 66));
  g.fillCircle(c, c, R - rw, tft.color565(14, 24, 36));
  const uint16_t arc = s_muted ? cDim() : cCyan();
  const float end_deg = -90.0f + 360.0f * level / steps;
  if (level >= steps) {
    g.fillCircle(c, c, R - S(1), arc);
    g.fillCircle(c, c, R - rw + S(1), tft.color565(14, 24, 36));
  } else if (level > 0) {
    g.fillArc(c, c, R - S(1), R - rw + S(1), -90.0f, end_deg, arc);
  }
  // Knob end: a dot on the ring at the current level (also at zero, at the top).
  const float a = end_deg * static_cast<float>(M_PI) / 180.0f;
  const int dr = R - rw / 2;
  g.fillCircle(c + lroundf(cosf(a) * dr), c + lroundf(sinf(a) * dr), rw / 2 + S(2), 0xFFFF);
  g.fillCircle(c + lroundf(cosf(a) * dr), c + lroundf(sinf(a) * dr), rw / 2 - S(1), arc);
  // Centre: speaker (crossed out while muted) and the level in percent.
  const uint16_t sc = s_muted ? cRed() : 0xFFFF;
  speakerIcon(g, c - S(6), c - S(22), S(12), sc);
  const int mx = c + S(10), my = c - S(22);
  if (s_muted) {
    for (int t = -1; t <= 1; ++t) {
      g.drawLine(mx - S(6) + t, my - S(6), mx + S(6) + t, my + S(6), sc);
      g.drawLine(mx - S(6) + t, my + S(6), mx + S(6) + t, my - S(6), sc);
    }
  } else {
    g.drawArc(mx - S(6), my, S(6), S(4), -45, 45, sc);
    g.drawArc(mx - S(6), my, S(11), S(9), -45, 45, sc);
  }
  char pct[8];
  snprintf(pct, sizeof(pct), "%%%d", (level * 100 + steps / 2) / steps);
  displayFontSetBitmap(g, &ui::font::SansBold24);
  g.setTextDatum(textdatum_t::middle_center);
  g.setTextColor(s_muted ? cDim() : 0xFFFF);
  g.drawString(pct, c, c + S(8));
  displayFontSetBitmap(g, &ui::font::SansBold8);
  g.setTextColor(s_muted ? cRed() : cDim());
  g.drawString(s_muted ? "SESSİZ" : "SES", c, c + S(36));
  g.pushSprite(kKnobX, kKnobY);
}

void drawUsbStatus() {
  const bool kb = console::keyboardMode();
  const bool ok = kb && services::hid::ready();
  s_usb_shown = ok ? 1 : (kb ? 0 : 2);
  const int w = S(230);
  const int x = kW - S(8) - w;
  tft.fillRect(x, 0, w, kTopH, cBg());
  tft.fillCircle(x + w - S(160), kTopH / 2, S(5),
                 ok ? tft.color565(80, 220, 120) : (kb ? cRed() : tft.color565(240, 180, 60)));
  displayFontSetBitmap(tft, &ui::font::Sans8);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(ok ? cDim() : tft.color565(230, 150, 130), cBg());
  tft.drawString(ok ? "USB bağlı" : (kb ? "USB bağlı değil" : "Kurulum modu: tuşlar kapalı"),
                 x + w - S(150), kTopH / 2 + 1);
  tft.setTextDatum(textdatum_t::top_left);
}

void drawBackButton() {
  const uint16_t panel = tft.color565(12, 26, 38);
  tft.fillRoundRect(ui::kSwitchX, ui::kSwitchY, ui::kSwitchW, ui::kSwitchH, S(8), panel);
  tft.drawRoundRect(ui::kSwitchX, ui::kSwitchY, ui::kSwitchW, ui::kSwitchH, S(8), cEdge());
  const int cx = ui::kSwitchX + S(21), cy = ui::kSwitchY + ui::kSwitchH / 2;
  tft.drawCircle(cx, cy, S(12), cCyan());
  tft.drawCircle(cx, cy, S(6), cCyan());
  tft.drawLine(cx, cy, cx + S(9), cy - S(8), cCyan());
  displayFontSetBitmap(tft, &ui::font::SansBold8);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(0xFFFF, panel);
  tft.drawString("RADAR", (ui::kSwitchX + S(38) + ui::kSwitchX + ui::kSwitchW) / 2, cy + 1);
  tft.setTextDatum(textdatum_t::top_left);
}

void drawAll() {
  tft.fillScreen(cBg());
  displayFontSetBitmap(tft, &ui::font::SansBold9);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(0xFFFF, cBg());
  tft.drawString("KONTROL", S(10), kTopH / 2 + 1);
  drawUsbStatus();
  for (int i = 0; i < store::kSlots; ++i) drawKey(i, false);
  drawKnob();
  displayFontSetBitmap(tft, &ui::font::Sans8);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(cDim(), cBg());
  tft.drawString("Tuşları kurulum sayfasındaki Kontrol tuşları bölümünden değiştirebilirsin", S(10),
                 ui::kSwitchY + ui::kSwitchH / 2);
  drawBackButton();
  tft.setTextDatum(textdatum_t::top_left);
  s_gen_shown = store::generation();
}

// --- Actions -------------------------------------------------------------------------

bool canSend() { return console::keyboardMode() && services::hid::ready(); }

void fireKey(int i) {
  const store::Slot& k = store::slot(i);
  Console.printf("Deck: %s\n", k.label);
  if (!canSend()) return;
  if (k.kind == store::Kind::Media) {
    services::hid::sendMedia(static_cast<Media>(k.code));
  } else if (k.kind == store::Kind::Key && k.code != 0) {
    services::hid::sendCombo(k.mods, k.code);
  }
}

void volumeStep(int dir) {
  s_level = constrain(s_level + dir, 0, services::deck::volumeSteps());
  s_level_changed_ms = millis();
  s_level_dirty = true;
  s_muted = false;  // Windows and macOS unmute on a volume key
  if (canSend()) services::hid::sendMedia(dir > 0 ? Media::VolumeUp : Media::VolumeDown);
}

float angleAt(int x, int y) {
  return atan2f(static_cast<float>(y - kKnobCy), static_cast<float>(x - kKnobCx)) * 180.0f /
         static_cast<float>(M_PI);
}

}  // namespace

void enter() {
  if (s_level < 0) loadLevel();
  s_pressed = -1;
  s_flash = false;
  s_down = false;
  s_target = Target::None;
  if (!s_knob_ok) {
    s_knob.setPsram(true);
    s_knob.setColorDepth(16);
    s_knob_ok = s_knob.createSprite(kKnobD, kKnobD) != nullptr;
  }
  if (s_icon_buf == nullptr) {
    const int px = store::iconPx();
    s_icon_buf = static_cast<uint16_t*>(heap_caps_malloc(px * px * 2, MALLOC_CAP_SPIRAM));
  }
  tft.beginHiddenFrame();
  drawAll();
  tft.presentHiddenFrame();
}

bool tick() {
  if (s_gen_shown != store::generation()) {
    enter();
    return true;
  }
  const unsigned long now = millis();
  if (s_flash && s_pressed >= 0 && now - s_pressed_ms >= kPressFlashMs) {
    drawKey(s_pressed, false);
    s_pressed = -1;
    s_flash = false;
  }
  if (s_level_dirty && now - s_level_changed_ms >= kSaveLevelMs) saveLevel();
  const int usb = console::keyboardMode() ? (services::hid::ready() ? 1 : 0) : 2;
  if (usb != s_usb_shown) drawUsbStatus();
  return true;
}

Touch touch(int fingers, int x, int y) {
  const unsigned long now = millis();
  if (fingers > 0) {
    s_last_x = x;
    s_last_y = y;
    if (!s_down) {
      s_down = true;
      s_down_x = x;
      s_down_y = y;
      s_down_ms = now;
      s_max_move = 0;
      if (s_flash && s_pressed >= 0) {  // a quick second press: clear the old highlight
        drawKey(s_pressed, false);
        s_pressed = -1;
        s_flash = false;
      }
      if (ui::switchButtonHit(x, y)) {
        s_target = Target::Back;
      } else if (inKnob(x, y)) {
        s_target = Target::Knob;
        s_last_deg = angleAt(x, y);
        s_accum_deg = s_total_deg = 0.0f;
      } else if ((s_key = keyAt(x, y)) >= 0) {
        s_target = Target::Key;
        drawKey(s_key, true);
        s_pressed = s_key;
      } else {
        s_target = Target::None;
      }
      return Touch::None;
    }
    const int move = abs(x - s_down_x) + abs(y - s_down_y);
    if (move > s_max_move) s_max_move = move;
    if (s_target == Target::Knob) {
      const int dx = x - kKnobCx, dy = y - kKnobCy;
      if (dx * dx + dy * dy < (kKnobR / 4) * (kKnobR / 4)) return Touch::None;  // centre: no angle
      const float deg = angleAt(x, y);
      float d = deg - s_last_deg;
      if (d > 180.0f) d -= 360.0f;
      if (d < -180.0f) d += 360.0f;
      s_last_deg = deg;
      s_accum_deg += d;
      s_total_deg += fabsf(d);
      bool changed = false;
      const float step = stepDeg();
      while (s_accum_deg >= step) {
        s_accum_deg -= step;
        volumeStep(+1);  // clockwise (screen y points down): louder
        changed = true;
      }
      while (s_accum_deg <= -step) {
        s_accum_deg += step;
        volumeStep(-1);
        changed = true;
      }
      if (changed) drawKnob();
    } else if (s_target == Target::Key && keyAt(x, y) != s_key && s_pressed == s_key) {
      drawKey(s_key, false);  // slid off the key: no press
      s_pressed = -1;
    }
    return Touch::None;
  }
  if (!s_down) return Touch::None;
  s_down = false;
  x = s_last_x;
  y = s_last_y;
  const Target t = s_target;
  s_target = Target::None;
  switch (t) {
    case Target::Back:
      return ui::switchButtonHit(x, y) || s_max_move < S(20) ? Touch::Back : Touch::None;
    case Target::Knob:
      if (s_total_deg < kTapMaxDeg && s_max_move < S(14) && now - s_down_ms < kTapMaxMs) {
        s_muted = !s_muted;
        if (canSend()) services::hid::sendMedia(Media::Mute);
        drawKnob();
      }
      return Touch::None;
    case Target::Key:
      if (s_pressed == s_key) {
        fireKey(s_key);
        s_pressed_ms = millis();
        s_flash = true;  // tick() lets go of the highlight
      }
      return Touch::None;
    case Target::None: return Touch::None;
  }
  return Touch::None;
}

}  // namespace ui::deck

#endif  // BOARD_WS5
