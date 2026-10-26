#include "ui/deck_screen.h"

#if defined(BOARD_WS5)

#include <Arduino.h>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/hid_keys.h"
#include "ui/fonts_tr.h"
#include "ui/screen_switch.h"
#include "ui/ui_scale.h"

namespace ui::deck {

namespace {

using services::hid::Media;

/** How long a pressed key stays highlighted. */
constexpr unsigned long kPressFlashMs = 140;

// Arduino keyboard codes for F13..F24 (USBHIDKeyboard.h: KEY_F13 = 0xF0 ... KEY_F24 = 0xFB).
constexpr uint8_t kF13 = 0xF0;

enum class Icon : uint8_t { None, Prev, PlayPause, Next, VolDown, VolUp, Mute };

struct Key {
  const char* label;  // shown under the icon, or alone
  Icon icon;
  bool media;
  Media media_key;
  uint8_t modifiers;
  uint8_t key;
};

// 5 x 3 grid. F13-F24 are not on normal keyboards, so they never clash with an app's
// own shortcuts: assign them in AutoHotkey, the app's shortcut settings, OBS, etc.
constexpr int kCols = 5;
constexpr int kRows = 3;
const Key kKeys[kCols * kRows] = {
    {"Önceki", Icon::Prev, true, Media::Previous, 0, 0},
    {"Oynat / Dur", Icon::PlayPause, true, Media::PlayPause, 0, 0},
    {"Sonraki", Icon::Next, true, Media::Next, 0, 0},
    {"Ses -", Icon::VolDown, true, Media::VolumeDown, 0, 0},
    {"Ses +", Icon::VolUp, true, Media::VolumeUp, 0, 0},

    {"Sessiz", Icon::Mute, true, Media::Mute, 0, 0},
    {"F13", Icon::None, false, Media::Mute, 0, kF13 + 0},
    {"F14", Icon::None, false, Media::Mute, 0, kF13 + 1},
    {"F15", Icon::None, false, Media::Mute, 0, kF13 + 2},
    {"F16", Icon::None, false, Media::Mute, 0, kF13 + 3},

    {"F17", Icon::None, false, Media::Mute, 0, kF13 + 4},
    {"F18", Icon::None, false, Media::Mute, 0, kF13 + 5},
    {"F19", Icon::None, false, Media::Mute, 0, kF13 + 6},
    {"F20", Icon::None, false, Media::Mute, 0, kF13 + 7},
    {"F21", Icon::None, false, Media::Mute, 0, kF13 + 8},
};

// Layout: title bar, key grid, RADAR button bottom right (same place as on the other screens).
constexpr int kW = config::kDisplayWidth;
constexpr int kTopH = S(34);
constexpr int kGap = S(8);
constexpr int kGridX = S(8);
constexpr int kGridY = kTopH + S(4);
constexpr int kGridW = kW - 2 * kGridX;
constexpr int kGridH = ui::kSwitchY - S(6) - kGridY;
constexpr int kKeyW = (kGridW - (kCols - 1) * kGap) / kCols;
constexpr int kKeyH = (kGridH - (kRows - 1) * kGap) / kRows;

int s_pressed = -1;
unsigned long s_pressed_ms = 0;
int s_usb_shown = -1;

uint16_t cBg() { return tft.color565(6, 10, 18); }
uint16_t cKey() { return tft.color565(18, 30, 46); }
uint16_t cKeyMedia() { return tft.color565(14, 38, 52); }
uint16_t cKeyDown() { return tft.color565(60, 150, 210); }
uint16_t cEdge() { return tft.color565(48, 86, 110); }
uint16_t cText() { return 0xFFFF; }
uint16_t cDim() { return tft.color565(125, 139, 150); }
uint16_t cCyan() { return tft.color565(110, 210, 255); }

void keyRect(int i, int* x, int* y) {
  *x = kGridX + (i % kCols) * (kKeyW + kGap);
  *y = kGridY + (i / kCols) * (kKeyH + kGap);
}

void speaker(int cx, int cy, int s, uint16_t c) {
  tft.fillRect(cx - s, cy - s / 2, s * 2 / 3, s, c);
  tft.fillTriangle(cx - s / 3, cy - s / 2, cx + s / 3, cy - s, cx + s / 3, cy + s, c);
  tft.fillRect(cx - s / 3, cy - s / 2, s * 2 / 3, s, c);
}

void drawIcon(Icon icon, int cx, int cy, uint16_t c, uint16_t bg) {
  const int s = S(14);
  switch (icon) {
    case Icon::Prev:
      tft.fillRect(cx - s, cy - s, S(3), 2 * s, c);
      tft.fillTriangle(cx - s + S(3), cy, cx, cy - s, cx, cy + s, c);
      tft.fillTriangle(cx, cy, cx + s, cy - s, cx + s, cy + s, c);
      break;
    case Icon::Next:
      tft.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx, cy, c);
      tft.fillTriangle(cx, cy - s, cx, cy + s, cx + s - S(3), cy, c);
      tft.fillRect(cx + s - S(3), cy - s, S(3), 2 * s, c);
      break;
    case Icon::PlayPause:
      tft.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + S(2), cy, c);
      tft.fillRect(cx + S(5), cy - s, S(4), 2 * s, c);
      tft.fillRect(cx + S(12), cy - s, S(4), 2 * s, c);
      break;
    case Icon::VolDown:
    case Icon::VolUp:
    case Icon::Mute: {
      speaker(cx - S(6), cy, s, c);
      const int mx = cx + S(12);
      if (icon == Icon::Mute) {
        for (int t = -1; t <= 1; ++t) {
          tft.drawLine(mx - S(5) + t, cy - S(5), mx + S(5) + t, cy + S(5), c);
          tft.drawLine(mx - S(5) + t, cy + S(5), mx + S(5) + t, cy - S(5), c);
        }
      } else {
        tft.fillRect(mx - S(6), cy - S(1), S(12), S(3), c);
        if (icon == Icon::VolUp) tft.fillRect(mx - S(1), cy - S(6), S(3), S(12), c);
      }
      (void)bg;
      break;
    }
    case Icon::None: break;
  }
}

void drawKey(int i, bool down) {
  int x, y;
  keyRect(i, &x, &y);
  const Key& k = kKeys[i];
  const uint16_t fill = down ? cKeyDown() : (k.media ? cKeyMedia() : cKey());
  tft.fillRoundRect(x, y, kKeyW, kKeyH, S(10), fill);
  tft.drawRoundRect(x, y, kKeyW, kKeyH, S(10), down ? cText() : cEdge());
  const int cx = x + kKeyW / 2, cy = y + kKeyH / 2;
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(cText(), fill);
  if (k.icon != Icon::None) {
    drawIcon(k.icon, cx, cy - S(10), down ? cText() : cCyan(), fill);
    displayFontSetBitmap(tft, &ui::font::Sans9);
    tft.drawString(k.label, cx, cy + S(22));
  } else {
    displayFontSetBitmap(tft, &ui::font::SansBold18);
    tft.drawString(k.label, cx, cy);
  }
}

void drawUsbStatus() {
  const bool ok = services::hid::ready();
  s_usb_shown = ok ? 1 : 0;
  const int w = S(150);
  const int x = kW - S(8) - w;
  tft.fillRect(x, 0, w, kTopH, cBg());
  tft.fillCircle(x + S(8), kTopH / 2, S(5), ok ? tft.color565(80, 220, 120) : tft.color565(230, 90, 70));
  displayFontSetBitmap(tft, &ui::font::Sans8);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(ok ? cDim() : tft.color565(230, 150, 130), cBg());
  tft.drawString(ok ? "USB bağlı" : "USB bağlı değil", x + S(18), kTopH / 2 + 1);
}

void drawBackButton() {
  const uint16_t panel = tft.color565(12, 26, 38);
  tft.fillRoundRect(ui::kSwitchX, ui::kSwitchY, ui::kSwitchW, ui::kSwitchH, S(8), panel);
  tft.drawRoundRect(ui::kSwitchX, ui::kSwitchY, ui::kSwitchW, ui::kSwitchH, S(8), cEdge());
  // radar rings
  const int cx = ui::kSwitchX + S(21), cy = ui::kSwitchY + ui::kSwitchH / 2;
  tft.drawCircle(cx, cy, S(12), cCyan());
  tft.drawCircle(cx, cy, S(6), cCyan());
  tft.drawLine(cx, cy, cx + S(9), cy - S(8), cCyan());
  displayFontSetBitmap(tft, &ui::font::SansBold8);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(cText(), panel);
  tft.drawString("RADAR", (ui::kSwitchX + S(38) + ui::kSwitchX + ui::kSwitchW) / 2, cy + 1);
}

void drawAll() {
  tft.fillScreen(cBg());
  displayFontSetBitmap(tft, &ui::font::SansBold9);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(cText(), cBg());
  tft.drawString("KONTROL", S(10), kTopH / 2 + 1);
  drawUsbStatus();
  for (int i = 0; i < kCols * kRows; ++i) drawKey(i, false);
  displayFontSetBitmap(tft, &ui::font::Sans8);
  tft.setTextDatum(textdatum_t::middle_left);
  tft.setTextColor(cDim(), cBg());
  tft.drawString("F13-F21: bilgisayarda istediğin kısayola ata", S(10),
                 ui::kSwitchY + ui::kSwitchH / 2);
  drawBackButton();
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

void enter() {
  s_pressed = -1;
  // Draw into the hidden buffer and show it in one go (no half-drawn screen).
  tft.beginHiddenFrame();
  drawAll();
  tft.presentHiddenFrame();
}

bool tick() {
  const unsigned long now = millis();
  if (s_pressed >= 0 && now - s_pressed_ms >= kPressFlashMs) {
    drawKey(s_pressed, false);
    s_pressed = -1;
  }
  if ((services::hid::ready() ? 1 : 0) != s_usb_shown) drawUsbStatus();
  return true;  // the deck stays until RADAR is tapped
}

Tap tap(int x, int y) {
  if (ui::switchButtonHit(x, y)) return Tap::Back;
  for (int i = 0; i < kCols * kRows; ++i) {
    int kx, ky;
    keyRect(i, &kx, &ky);
    if (x < kx || x >= kx + kKeyW || y < ky || y >= ky + kKeyH) continue;
    if (s_pressed >= 0 && s_pressed != i) drawKey(s_pressed, false);
    drawKey(i, true);  // feedback first, then the (up to ~15 ms) USB report
    s_pressed = i;
    s_pressed_ms = millis();
    const Key& k = kKeys[i];
    if (k.media) {
      services::hid::sendMedia(k.media_key);
    } else {
      services::hid::sendCombo(k.modifiers, k.key);
    }
    Serial.printf("Deck: %s\n", k.label);
    return Tap::Key;
  }
  return Tap::None;
}

}  // namespace ui::deck

#endif  // BOARD_WS5
