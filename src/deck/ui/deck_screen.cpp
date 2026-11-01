#include "ui/deck_screen.h"

#if defined(BOARD_WS5)

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "config.h"
#include "hardware/console.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/deck_store.h"
#include "services/hid_keys.h"
#include "services/host_link.h"
#include "services/perf.h"
#include "ui/fonts_tr.h"
#include "ui/paint.h"
#include "ui/ui_text.h"

namespace ui::deck {

namespace {

using services::hid::Media;
using paint::Rgb;
namespace store = services::deck;
namespace host = services::host;

// ------------------------------------------------------------------ layout
// The approved design is drawn at 800 x 480, the panel's own size.
constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;
constexpr int D(int v) { return v * kW / 800; }
constexpr float Df(float v) { return v * kW / 800.0f; }

// Shared top bar: RADAR pill on the right, page dots in the middle.
constexpr int kRadarW = D(112), kRadarH = D(34), kRadarX = D(780) - kRadarW, kRadarY = D(15);
// Page switch at the top: two halves, MÜZİK | TUŞLAR, tapped to change the page.
constexpr int kTabW = D(176), kTabH = D(34), kTabX = (kW - kTabW) / 2, kTabY = D(15);
constexpr int kTopCy = kTabY + kTabH / 2;  // middle line of the top bar

// Keys page: 4 x 3 keys on the left, the volume knob on the right.
constexpr int kCols = 4, kRows = 3;
constexpr int kGridX = D(20), kGridY = D(68), kGap = D(10);
constexpr int kKeyW = store::keyW();  // (D(470) - 3 * D(10)) / 4
constexpr int kKeyH = store::keyH();  // D(120)
constexpr int kKnobX = D(512), kKnobY = D(88), kKnobD = D(268);
constexpr int kKnobCx = kKnobX + kKnobD / 2, kKnobCy = kKnobY + kKnobD / 2;
constexpr float kRingR = kKnobD / 2.0f, kRingW = D(14);
static_assert(kCols * kRows == store::kSlots, "one key per stored slot");
// Previous / play-pause / next under the knob: always there, whatever the keys are.
constexpr int kKCtrlY = D(410);
constexpr int kKPrevCx = kKnobCx - D(78), kKPlayCx = kKnobCx, kKNextCx = kKnobCx + D(78);
constexpr int kKSmallR = D(24), kKPlayR = D(30);

// Music page: text, bars and controls at the bottom, over the cover.
constexpr int kMusicPad = D(28);
constexpr int kTitleY = D(294);  // top of the title line; the artist follows under it
constexpr int kBarY = D(374), kBarH = D(6);
constexpr int kTimeW = D(46);
constexpr int kCtrlY = D(436);
constexpr int kPrevCx = D(314), kPlayCx = D(400), kNextCx = D(486);
constexpr int kSmallBtnR = D(26), kPlayR = D(34);
// Volume bar left of the controls: thick, a big handle, a tall touch area (below the
// time bar, which takes no touches).
constexpr int kVolX = D(66), kVolW = D(200), kVolH = D(10), kVolKnobR = D(13);
constexpr int kVolTouchH = D(40);  // either side of kCtrlY
constexpr Rgb kCoverBg{6, 7, 14};  // what the cover fades into

// Queue ("Sıradakiler"): the next song bottom right of the music page; tapped, a panel
// on the right lists the queue and the playing song moves to a smaller left column.
constexpr int kNextX = D(572), kNextY = D(410), kNextW = D(200), kNextH = D(52);
constexpr int kQPanelX = D(448), kQPanelY = D(64), kQPanelW = D(332), kQPanelH = D(400);
constexpr int kQHeadH = D(50), kQRowH = D(58);
constexpr int kQRows = (kQPanelH - kQHeadH) / kQRowH;  // rows in view
constexpr int kQCloseCx = kQPanelX + kQPanelW - D(30), kQCloseCy = kQPanelY + kQHeadH / 2;
constexpr int kQCoverX = D(28), kQCoverY = D(74), kQCoverS = D(156);
constexpr int kQLeftW = D(392);
constexpr int kQBarY = D(334);
constexpr int kQCtrlY = D(404), kQPrevCx = D(152), kQPlayCx = D(224), kQNextCx = D(296);
constexpr int kQSmallR = D(24), kQPlayR = D(31);

// Colours (approved palette).
constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kText{243, 244, 251};
constexpr Rgb kInk{8, 10, 24};
constexpr Rgb kPeach{255, 180, 140};
constexpr Rgb kPink{243, 166, 182};
constexpr Rgb kLavender{185, 166, 255};
constexpr Rgb kFaceA{38, 43, 87}, kFaceB{21, 25, 56};
constexpr Rgb kMutedRed{245, 120, 120};

uint16_t col565(Rgb c) { return tft.color565(c.r, c.g, c.b); }

// ------------------------------------------------------------------ state
enum class Page { Music, Keys };
Page s_page = Page::Keys;

LGFX_Sprite s_canvas(&tft);      // only for an upside-down screen
LGFX_Sprite s_keys_base(&tft);   // keys page without the moving parts
// Cover (or background) with its shades; with the queue open, the small cover and the panel
// glass instead: a third screen (768 KB) left the queue page short of PSRAM (stage 10b-4).
LGFX_Sprite s_music_base(&tft);
bool s_ready = false;
bool s_keys_dirty = true, s_music_dirty = true;
uint32_t s_keys_gen = 0, s_cover_gen = 0, s_now_gen = 0;
uint32_t s_cover_for_id = 0;  // track the music base was built for
bool s_music_card = false;    // the music base has the next card's glass (stage 10d-1)

struct RingPx {
  uint16_t x, y;  // canvas position
  uint16_t ang;   // 0..36000 (0.01 degree, 0 = top, clockwise)
  uint8_t cov;    // radial coverage of the ring band
  uint8_t pad;
};
RingPx* s_ring = nullptr;
int s_ring_n = 0;

// Volume: from the helper when it runs, else counted locally from the keys sent.
int s_level = -1;  // 0..volumeSteps() without the helper
bool s_muted_local = false;
unsigned long s_level_changed_ms = 0;
bool s_level_dirty = false;
int s_shown_level = -1;  // last level drawn (percent)
bool s_shown_muted = false;

// Key press / touch.
int s_pressed = -1;
unsigned long s_pressed_ms = 0;
bool s_flash = false;
enum class Target { None, Key, Knob, Back, Prev, Play, Next, VolBar, Swipe, Tab, QOpen, QClose, QList };
Target s_target = Target::None;
bool s_down = false;
int s_key = -1;
int s_down_x = 0, s_down_y = 0, s_last_x = 0, s_last_y = 0;
unsigned long s_down_ms = 0;
float s_last_deg = 0, s_accum_deg = 0, s_total_deg = 0;
int s_max_move = 0;
int s_target_level = -1;  // knob / bar with the helper, not sent yet (percent)
unsigned long s_last_send_ms = 0;
// While the finger is on the knob / bar (and a moment after) the level shown is the one
// asked for: the computer's answers lag behind and would make the ring flicker.
int s_user_level = -1;
unsigned long s_user_until_ms = 0;
float s_bar_x = -1;  // smoothed finger position on the volume bar

// Music page.
bool s_was_playing = false;
uint32_t s_last_id = 0;
unsigned long s_stopped_ms = 0;
uint32_t s_shown_pos = 0xFFFFFFFF;
uint32_t s_base_done_us = 0;  // the frame's page base copied (stage 10c timing)
// The frame's moving parts by kind (stage 10c): tabs, text, bars, controls, next card / panel.
uint32_t s_front_us[5] = {0, 0, 0, 0, 0}, s_front_last_us = 0;
enum FrontPart { kFpTabs, kFpText, kFpBars, kFpCtrls, kFpPanel };
void frontPart(FrontPart k) {
  const uint32_t t = micros();
  s_front_us[k] += t - s_front_last_us;
  s_front_last_us = t;
}
// The next card / queue panel by kind (stage 10d-0): base copied, pictures decoded, glass,
// pictures, text.
uint32_t s_card_us[5] = {0, 0, 0, 0, 0}, s_card_last_us = 0;
enum CardPart { kCpCopy, kCpDecode, kCpGlass, kCpPics, kCpText };
void cardStart() { s_card_last_us = micros(); }
void cardPart(CardPart k) {
  const uint32_t t = micros();
  s_card_us[k] += t - s_card_last_us;
  s_card_last_us = t;
}

// Queue panel.
bool s_queue_open = false;
int s_qscroll = 0, s_qscroll_down = 0;  // first row in view
bool s_qbase_dirty = true;
uint32_t s_qbase_cover_gen = 0, s_qbase_id = 0, s_shown_qver = 0;
int s_qflash = -1;
unsigned long s_qflash_ms = 0;
// A row picked from the list: when the new list comes with it on top, the panel goes back to
// the top (stage 10f). A song that changes on its own leaves the panel where it is.
char s_qpick[sizeof(host::QueueRow::title)] = "";
unsigned long s_qpick_ms = 0;
uint16_t* s_thumb[host::kQueueMax] = {};
const uint8_t* s_thumb_src[host::kQueueMax] = {};

// The queue panel's rows, kept: drawn again only when the list, the scroll, the highlight or
// the page under them changes (drawing them every frame held the page at ~8 frames a second).
LGFX_Sprite s_qpanel;
uint32_t s_qbase_builds = 0;
struct PanelKey {
  uint32_t ver, builds;
  int scroll, flash;
  bool operator==(const PanelKey& o) const {
    return ver == o.ver && builds == o.builds && scroll == o.scroll && flash == o.flash;
  }
};
PanelKey s_qpanel_key{0xFFFFFFFF, 0, -1, -1};

bool s_dirty = true;  // canvas needs to be put together and shown
unsigned long s_last_present_ms = 0;

paint::Canvas cv(LGFX_Sprite& s) { return {static_cast<uint16_t*>(s.getBuffer()), kW, kH}; }

// What each of the three panel buffers holds (stage 10c-3): the base it was put together on
// and a key per moving part, so a frame copies and draws only the parts changed since that
// buffer was drawn last (a whole frame was ~80-100 ms, the base copy alone 37).
constexpr int kParts = 5;
struct FbState {
  uint32_t base = 0;  // 0: unknown
  uint32_t part[kParts] = {};
};
FbState s_fb[3];
uint32_t s_base_serial = 0;  // bumped whenever a page base is built

struct Frame {
  paint::Canvas c, base;
  FbState* fb;  // nullptr: not kept track of (upside-down screen)
  bool full;    // the whole base was copied: every part is drawn
};

/** Part k needs drawing (its area then holds the base again) unless this buffer has key. */
bool part(Frame& f, int k, uint32_t key, int x, int y, int w, int h) {
  if (!f.full && f.fb->part[k] == key) return false;
  if (!f.full) paint::copyRect(f.c, f.base, x, y, w, h);
  if (f.fb != nullptr) f.fb->part[k] = key;
  return true;
}

constexpr uint32_t kSeed = 2166136261u;  // FNV-1a
uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
uint32_t mixStr(uint32_t h, const char* s) {
  while (*s) h = mix(h, static_cast<uint8_t>(*s++));
  return mix(h, 0);
}

// ------------------------------------------------------------------ volume
int steps() { return store::volumeSteps(); }

void loadLevel() {
  Preferences p;
  int v = steps() / 2;
  if (p.begin("deck", true)) {
    v = p.getInt("vol", v);
    p.end();
  }
  s_level = constrain(v, 0, steps());
}

void saveLevel() {
  Preferences p;
  if (p.begin("deck", false)) {
    p.putInt("vol", s_level);
    p.end();
  }
  s_level_dirty = false;
}

bool userHold() {
  return s_user_level >= 0 &&
         ((s_down && (s_target == Target::Knob || s_target == Target::VolBar)) || millis() < s_user_until_ms);
}

int levelPercent() {
  if (host::active()) return userHold() ? s_user_level : host::volume();
  return (constrain(s_level, 0, steps()) * 100 + steps() / 2) / steps();
}
bool mutedNow() { return host::active() ? host::muted() : s_muted_local; }
bool canSendHid() { return console::keyboardMode() && services::hid::ready(); }

/** One local volume step (no helper): a volume key, the count follows. */
void hidStep(int dir) {
  s_level = constrain(s_level + dir, 0, steps());
  s_level_changed_ms = millis();
  s_level_dirty = true;
  s_muted_local = false;
  if (canSendHid()) services::hid::sendMedia(dir > 0 ? Media::VolumeUp : Media::VolumeDown);
}

/** Absolute level wanted (percent): to the helper, else as volume keys. */
void wantLevel(int pct) {
  pct = constrain(pct, 0, 100);
  if (host::active()) {
    if (pct == s_user_level && userHold()) return;
    s_user_level = pct;
    s_user_until_ms = millis() + 1500;
    if (millis() - s_last_send_ms >= 40) {
      host::sendVolume(pct);  // also the level shown until the helper answers
      s_last_send_ms = millis();
      s_target_level = -1;
    } else {
      s_target_level = pct;  // tick() sends it
    }
    return;
  }
  const int want = (pct * steps() + 50) / 100;
  while (s_level < want) hidStep(+1);
  while (s_level > want) hidStep(-1);
}

void toggleMute() {
  if (host::active()) {
    host::sendMute();
  } else {
    s_muted_local = !s_muted_local;
    if (canSendHid()) services::hid::sendMedia(Media::Mute);
  }
}

// ------------------------------------------------------------------ drawing helpers
// Where each slot's key is: the fixed 4 x 3 grid, or (version 3 sets) only the used keys
// filling the area, an unfinished last row centred.
struct Box {
  int16_t x = 0, y = 0, w = 0, h = 0;
  bool shown = false;
};
Box s_box[store::kSlots];
uint32_t s_box_gen = 0;

void buildBoxes() {
  if (s_box_gen == store::generation()) return;
  s_box_gen = store::generation();
  for (auto& b : s_box) b = Box{};
  if (!store::compactLayout()) {
    for (int i = 0; i < store::kSlots; ++i) {
      s_box[i] = {static_cast<int16_t>(kGridX + (i % kCols) * (kKeyW + kGap)),
                  static_cast<int16_t>(kGridY + (i / kCols) * (kKeyH + kGap)), kKeyW, kKeyH, true};
    }
    return;
  }
  const int cols = store::layoutCols(), kw = store::layoutKeyW(), kh = store::layoutKeyH();
  int used = 0;
  for (int i = 0; i < store::kSlots; ++i) used += store::slotUsed(i) ? 1 : 0;
  int p = 0;
  for (int i = 0; i < store::kSlots; ++i) {
    if (!store::slotUsed(i)) continue;
    const int row = p / cols, col = p % cols;
    const int in_row = std::min(cols, used - row * cols);
    const int x0 = kGridX + (store::keyAreaW() - (in_row * kw + (in_row - 1) * kGap)) / 2;
    s_box[i] = {static_cast<int16_t>(x0 + col * (kw + kGap)), static_cast<int16_t>(kGridY + row * (kh + kGap)),
                static_cast<int16_t>(kw), static_cast<int16_t>(kh), true};
    ++p;
  }
}

const Box& keyBox(int i) {
  buildBoxes();
  return s_box[i];
}

int keyAt(int x, int y) {
  buildBoxes();
  for (int i = 0; i < store::kSlots; ++i) {
    const Box& b = s_box[i];
    if (b.shown && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
  }
  return -1;
}

bool inRadar(int x, int y) {
  return x >= kRadarX - D(10) && x < kRadarX + kRadarW + D(10) && y >= 0 && y < kRadarY + kRadarH + D(10);
}

bool inKnob(int x, int y) {
  const int dx = x - kKnobCx, dy = y - kKnobCy;
  return dx * dx + dy * dy <= (kKnobD / 2 + D(10)) * (kKnobD / 2 + D(10));
}

bool inCircle(int x, int y, int cx, int cy, int r) {
  const int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

namespace text = ui::text;

// Text colours.
constexpr Rgb kTextSoft{226, 229, 245}, kTextMuted{154, 161, 189}, kTextSub{184, 189, 214};

void radarPill(const paint::Canvas& c) {
  paint::roundRect(c, kRadarX, kRadarY, kRadarW, kRadarH, kRadarH / 2, kWhite, 22, kWhite, 34);
  const float cx = kRadarX + D(22), cy = kRadarY + kRadarH / 2.0f;
  paint::arc(c, cx, cy, Df(9), Df(7.4f), 0, 360, kWhite, 255);
  paint::arc(c, cx, cy, Df(4.5f), Df(3), 0, 360, kWhite, 255);
  paint::arc(c, cx, cy, Df(7.4f), 0, 20, 70, kWhite, 110);  // the sweep
  text::drawMid(c, text::Ui13b, "RADAR", kRadarX + D(38), kTopCy, kWhite, 255, text::Align::Left, 1);
}

/** 0 = not on the page switch, 1 = its music half, 2 = its keys half. */
int tabAt(int x, int y) {
  if (y < 0 || y >= kTabY + kTabH + D(14) || x < kTabX - D(10) || x >= kTabX + kTabW + D(10)) return 0;
  return x < kTabX + kTabW / 2 ? 1 : 2;
}

void pageTabs(const paint::Canvas& c, bool music_active) {
  const int half = kTabW / 2, pad = D(3);
  paint::roundRect(c, kTabX, kTabY, kTabW, kTabH, kTabH / 2, kWhite, 20, kWhite, 36);
  const int ax = music_active ? kTabX + pad : kTabX + half;
  paint::roundRect(c, ax, kTabY + pad, half - pad, kTabH - 2 * pad, (kTabH - 2 * pad) / 2, kWhite, 255);
  const Rgb ink{21, 24, 58};
  text::drawMid(c, text::Ui13b, "MÜZİK", kTabX + half / 2 + pad / 2, kTopCy, music_active ? ink : kTextSoft, 255,
                text::Align::Center, 1);
  text::drawMid(c, text::Ui13b, "TUŞLAR", kTabX + half + half / 2 - pad / 2, kTopCy, music_active ? kTextSoft : ink,
                255, text::Align::Center, 1);
}

template <typename Gfx>
void speakerIcon(Gfx& g, int cx, int cy, int s, uint16_t c) {
  g.fillRect(cx - s, cy - s / 2, s * 2 / 3, s, c);
  g.fillTriangle(cx - s / 3, cy - s / 2, cx + s / 3, cy - s, cx + s / 3, cy + s, c);
  g.fillRect(cx - s / 3, cy - s / 2, s * 2 / 3 + 1, s, c);
}

template <typename Gfx>
void mediaIcon(Gfx& g, Media m, int cx, int cy, uint16_t c) {
  const int s = D(13);
  switch (m) {
    case Media::Previous:
      g.fillRoundRect(cx - s, cy - s, D(4), 2 * s, D(1), c);
      g.fillTriangle(cx - s + D(3), cy, cx + s, cy - s, cx + s, cy + s, c);
      break;
    case Media::Next:
      g.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + s - D(3), cy, c);
      g.fillRoundRect(cx + s - D(4), cy - s, D(4), 2 * s, D(1), c);
      break;
    case Media::PlayPause:
      g.fillTriangle(cx - s, cy - s, cx - s, cy + s, cx + D(3), cy, c);
      g.fillRoundRect(cx + D(6), cy - s, D(4), 2 * s, D(1), c);
      g.fillRoundRect(cx + D(13), cy - s, D(4), 2 * s, D(1), c);
      break;
    case Media::VolumeUp:
    case Media::VolumeDown:
    case Media::Mute: {
      speakerIcon(g, cx - D(6), cy, s, c);
      const int mx = cx + D(12);
      if (m == Media::Mute) {
        for (int t = -1; t <= 1; ++t) {
          g.drawLine(mx - D(5) + t, cy - D(5), mx + D(5) + t, cy + D(5), c);
          g.drawLine(mx - D(5) + t, cy + D(5), mx + D(5) + t, cy - D(5), c);
        }
      } else {
        g.fillRect(mx - D(6), cy - D(1), D(12), D(3), c);
        if (m == Media::VolumeUp) g.fillRect(mx - D(1), cy - D(6), D(3), D(12), c);
      }
      break;
    }
  }
}

/** A stored picture onto a sprite: magenta = see-through. */
void blitIcon(LGFX_Sprite& s, const uint16_t* pic, int pw, int ph, int x, int y) {
  auto* dst = static_cast<uint16_t*>(s.getBuffer());
  for (int r = 0; r < ph; ++r) {
    const int yy = y + r;
    if (yy < 0 || yy >= kH) continue;
    for (int q = 0; q < pw; ++q) {
      const uint16_t v = pic[r * pw + q];
      const int xx = x + q;
      if (v == store::kTransparent || xx < 0 || xx >= kW) continue;
      dst[yy * kW + xx] = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
  }
}

void drawKeyContent(LGFX_Sprite& s, int i) {
  const Box& bx = keyBox(i);
  const int x = bx.x, y = bx.y, bw = bx.w, bh = bx.h;  // this key's size
  const auto c = cv(s);
  const store::Slot& k = store::slot(i);
  const int cx = x + bw / 2;
  const uint16_t* pic = store::icon(i);
  const int px = store::iconPx();
  const bool has_label = k.label[0] != '\0';
  char t[64];
  if (pic != nullptr && store::fullKeyIcons()) {
    // The picture is the whole key (the helper rounded its corners and darkened its
    // bottom for the label).
    blitIcon(s, pic, bw, bh, x, y);
    if (has_label) {
      text::fit(text::Ui13, k.label, bw - D(14), t, sizeof(t));
      text::draw(c, text::Ui13, t, cx + 1, y + bh - D(11) + 1, {4, 5, 12}, 160, text::Align::Center);
      text::draw(c, text::Ui13, t, cx, y + bh - D(11), kWhite, 255, text::Align::Center);
    }
  } else if (pic != nullptr) {
    const int iy = has_label ? y + (bh - px - D(22)) / 2 : y + (bh - px) / 2;
    blitIcon(s, pic, px, px, cx - px / 2, iy);
    if (has_label) {
      text::fit(text::Ui13, k.label, bw - D(12), t, sizeof(t));
      text::draw(c, text::Ui13, t, cx, y + bh - D(12), {217, 220, 239}, 255, text::Align::Center);
    }
  } else if (k.kind == store::Kind::Media) {
    mediaIcon(s, static_cast<Media>(k.code), cx, y + bh / 2 - D(10), col565({233, 234, 255}));
    text::fit(text::Ui13, k.label, bw - D(12), t, sizeof(t));
    text::draw(c, text::Ui13, t, cx, y + bh - D(14), {217, 220, 239}, 255, text::Align::Center);
  } else if (has_label) {
    // The largest size that fits (big keys of a short set get big letters).
    const text::Font* fonts[] = {&text::Ui22b, &text::Ui18b, &text::Ui15b, &text::Ui13b};
    const text::Font* f = fonts[3];
    for (const auto* cand : fonts) {
      if ((cand == &text::Ui22b && bh < D(150)) || text::width(*cand, k.label) > bw - D(16)) continue;
      f = cand;
      break;
    }
    text::fit(*f, k.label, bw - D(14), t, sizeof(t));
    text::drawMid(c, *f, t, cx, y + bh / 2, kText, 255, text::Align::Center);
  }
}

void buildRing() {
  if (s_ring != nullptr) return;
  std::vector<RingPx> v;
  const float ro = kRingR, ri = kRingR - kRingW;
  for (int y = kKnobY - 1; y <= kKnobY + kKnobD + 1; ++y) {
    for (int x = kKnobX - 1; x <= kKnobX + kKnobD + 1; ++x) {
      const float dx = x + 0.5f - kKnobCx, dy = y + 0.5f - kKnobCy;
      const float d = sqrtf(dx * dx + dy * dy);
      const float cov =
          std::min(std::max(ro - d + 0.5f, 0.0f), 1.0f) * std::min(std::max(d - ri + 0.5f, 0.0f), 1.0f);
      if (cov <= 0.0f) continue;
      float a = atan2f(dx, -dy) * 180.0f / static_cast<float>(M_PI);
      if (a < 0) a += 360.0f;
      v.push_back({static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(a * 100.0f),
                   static_cast<uint8_t>(cov * 255.0f + 0.5f), 0});
    }
  }
  s_ring = static_cast<RingPx*>(heap_caps_malloc(v.size() * sizeof(RingPx), MALLOC_CAP_SPIRAM));
  if (s_ring == nullptr) return;
  memcpy(s_ring, v.data(), v.size() * sizeof(RingPx));
  s_ring_n = static_cast<int>(v.size());
}

/** Knob face and the empty ring track (part of the keys base). */
void drawKnobBase(LGFX_Sprite& s) {
  const auto c = cv(s);
  paint::arc(c, kKnobCx, kKnobCy, kRingR, kRingR - kRingW, 0, 360, kWhite, 26);
  // Face: a soft radial shading, lighter towards the upper left.
  const float r = kRingR - kRingW, lx = kKnobCx - r * 0.2f, ly = kKnobCy - r * 0.36f;
  uint16_t* px = c.px;
  for (int y = static_cast<int>(kKnobCy - r - 1); y <= kKnobCy + r + 1; ++y) {
    if (y < 0 || y >= kH) continue;
    for (int x = static_cast<int>(kKnobCx - r - 1); x <= kKnobCx + r + 1; ++x) {
      const float dx = x + 0.5f - kKnobCx, dy = y + 0.5f - kKnobCy;
      const float cov = std::min(std::max(r - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f), 1.0f);
      if (cov <= 0.0f) continue;
      const float ex = x - lx, ey = y - ly;
      const float t = std::min(sqrtf(ex * ex + ey * ey) / (r * 1.35f), 1.0f);
      const float u = t * t * (3.0f - 2.0f * t);
      const float cr = kFaceA.r + (kFaceB.r - kFaceA.r) * u, cg = kFaceA.g + (kFaceB.g - kFaceA.g) * u,
                  cb = kFaceA.b + (kFaceB.b - kFaceA.b) * u;
      int r0, g0, b0;
      paint::unpack(px[y * kW + x], &r0, &g0, &b0);
      px[y * kW + x] = paint::pack(static_cast<int>(r0 + (cr - r0) * cov + 0.5f),
                                   static_cast<int>(g0 + (cg - g0) * cov + 0.5f),
                                   static_cast<int>(b0 + (cb - b0) * cov + 0.5f), x, y);
    }
  }
}

/** The moving part of the knob: filled arc, its end dot, the speaker. */
template <typename Gfx>
void drawKnobLevel(Gfx& s, const paint::Canvas& c, int pct, bool muted) {
  uint16_t* px = c.px;
  const int lvl = constrain(pct, 0, 100) * 360;  // 0.01 degree
  const float edge = 57.3f / kRingR * 100.0f;    // one pixel of arc, in 0.01 degree
  const Rgb dim{150, 156, 190};
  for (int i = 0; i < s_ring_n; ++i) {
    const RingPx& p = s_ring[i];
    if (p.ang > lvl + edge) continue;
    float cov = p.cov / 255.0f;
    if (p.ang > lvl - edge) cov *= std::min(std::max((lvl - p.ang) / edge + 0.5f, 0.0f), 1.0f);
    if (cov <= 0.0f) continue;
    const Rgb col =
        muted ? dim : paint::ramp(kPeach, kPink, kLavender, lvl > 0 ? static_cast<float>(p.ang) / lvl : 0.0f);
    uint16_t& d = px[p.y * kW + p.x];
    int r, g, b;
    paint::unpack(d, &r, &g, &b);
    d = paint::pack(static_cast<int>(r + (col.r - r) * cov), static_cast<int>(g + (col.g - g) * cov),
                    static_cast<int>(b + (col.b - b) * cov), p.x, p.y);
  }
  const float a = (lvl / 100.0f) * static_cast<float>(M_PI) / 180.0f;
  const float mr = kRingR - kRingW / 2.0f;
  const float ex = kKnobCx + sinf(a) * mr, ey = kKnobCy - cosf(a) * mr;
  paint::circle(c, ex, ey, D(15), muted ? dim : kLavender, 110);
  paint::circle(c, ex, ey, D(11), kWhite, 255);
  // Speaker in the middle, waves or a cross.
  const uint16_t sc = col565(muted ? kMutedRed : kText);
  speakerIcon(s, kKnobCx - D(12), kKnobCy, D(22), sc);
  const int mx = kKnobCx + D(16);
  if (muted) {
    for (int t = -2; t <= 2; ++t) {
      s.drawLine(mx - D(9) + t, kKnobCy - D(9), mx + D(9) + t, kKnobCy + D(9), sc);
      s.drawLine(mx - D(9) + t, kKnobCy + D(9), mx + D(9) + t, kKnobCy - D(9), sc);
    }
  } else {
    paint::arc(c, mx - D(8), kKnobCy, D(11), D(8), 50, 130, kText, 255);
    paint::arc(c, mx - D(8), kKnobCy, D(21), D(18), 50, 130, kText, 255);
  }
}

// ------------------------------------------------------------------ pages
/** Previous, play / pause, next: round glass buttons (play filled white). */
template <typename Gfx>
void drawTransport(Gfx& s, const paint::Canvas& c, int prev_cx, int play_cx, int next_cx, int cy, int small_r,
                   int play_r) {
  const auto& n = host::now();
  paint::circle(c, prev_cx, cy, small_r, kWhite, 26);
  paint::circle(c, next_cx, cy, small_r, kWhite, 26);
  paint::circle(c, play_cx, cy, play_r, kWhite, 255);
  mediaIcon(s, Media::Previous, prev_cx, cy, col565(kWhite));
  mediaIcon(s, Media::Next, next_cx, cy, col565(kWhite));
  const uint16_t ink = col565({21, 24, 58});
  const int u = play_r * 10 / 34;  // icon unit, scaled with the button
  if (n.valid && n.playing) {
    s.fillRoundRect(play_cx - u - u * 7 / 10, cy - u * 12 / 10, u * 7 / 10, u * 24 / 10, D(2), ink);
    s.fillRoundRect(play_cx + u * 3 / 10, cy - u * 12 / 10, u * 7 / 10, u * 24 / 10, D(2), ink);
  } else {
    s.fillTriangle(play_cx - u * 8 / 10, cy - u * 13 / 10, play_cx - u * 8 / 10, cy + u * 13 / 10,
                   play_cx + u * 13 / 10, cy, ink);
  }
}

void buildKeysBase() {
  auto& s = s_keys_base;
  const auto c = cv(s);
  paint::backgroundFast(c);
  text::drawMid(c, text::Ui13b, "KONTROL", D(20), kTopCy, {230, 232, 246}, 255, text::Align::Left, 1);
  if (!console::keyboardMode()) {
    text::drawMid(c, text::Ui13, "Kurulum modu: tuşlar kapalı", kW - D(20), kH - D(16), {245, 190, 120}, 255,
                  text::Align::Right);
  }
  pageTabs(c, false);
  radarPill(c);
  for (int i = 0; i < store::kSlots; ++i) {
    const Box& b = keyBox(i);
    if (!b.shown) continue;
    const store::Slot& k = store::slot(i);
    if (k.kind == store::Kind::Empty && !k.label[0] && !store::icon(i)) {
      paint::roundRect(c, b.x, b.y, b.w, b.h, D(18), kWhite, 0, kWhite, 40);
      continue;
    }
    paint::roundRect(c, b.x, b.y, b.w, b.h, D(18), kWhite, 15, kWhite, 26);
    drawKeyContent(s, i);
  }
  drawKnobBase(s);
  text::drawMid(c, text::Ui13, "Sağa kaydır: müzik", D(20), kH - D(16), kTextMuted);
  s_keys_gen = store::generation();
  s_keys_dirty = false;
  ++s_base_serial;
}

float smooth01(float t) {
  t = std::min(std::max(t, 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/** Byte order of a decoded 24-bit sprite (B G R in memory?), from pixels whose red and blue
 *  differ: the fast page code reads the sprite's memory directly. */
bool coverIsBgr(LGFX_Sprite& pic, int cw, int ch) {
  static int s_known = -1;  // the library's order does not change: found once
  if (s_known >= 0) return s_known == 1;
  const auto* buf = static_cast<const uint8_t*>(pic.getBuffer());
  for (int i = 0; i < 64; ++i) {
    const int x = (cw - 1) * i / 63, y = (ch - 1) * ((i * 37) % 64) / 63;
    const auto p = pic.readPixelRGB(x, y);
    if (p.R8() == p.B8()) continue;
    const uint8_t* q = buf + (y * cw + x) * 3;
    if (q[0] == p.B8() && q[2] == p.R8()) return (s_known = 1) == 1;
    if (q[0] == p.R8() && q[2] == p.B8()) return (s_known = 0) == 1;
  }
  return true;  // a grey cover: either order looks the same (asked again next time)
}

/** True when the stored cover belongs to what is playing now. */
bool coverForNow() {
  size_t len = 0;
  const auto& n = host::now();
  return host::cover(&len) != nullptr && len > 0 && n.valid && (host::coverId() == n.id || n.id == 0);
}

// ------------------------------------------------------------------ cover builds on core 0
// A page base with the cover takes ~0.7 s (stage 10b-5). It is built on core 0 so the loop
// on core 1 keeps reading touches and the cable; meanwhile nothing reads the music base and
// the screen keeps its last frame (stage 10b B-1).
enum class Job : uint8_t { Idle, Running, Done };
struct CoverJob {
  bool queue;          // the queue's small cover, else the music page
  const uint8_t* jpg;  // nullptr: no cover for what plays, the background only
  size_t len;
  int cw, ch;
  uint32_t gen, id;    // the cover generation and track it was built for
  uint32_t dec_us, total_us;
  bool card;           // music page: with the next card's glass
};
CoverJob s_job{};
std::atomic<Job> s_job_state{Job::Idle};
TaskHandle_t s_job_task = nullptr;

/** The music page shows the next song's card (else a hint in its place). */
bool nextCardShown() { return host::queueValid() && host::queueCount() > 1; }

/** The next card's glass: in the music base, or drawn with the card when the base has none. */
void nextCardGlass(const paint::Canvas& c) {
  paint::roundRect(c, kNextX, kNextY, kNextW, kNextH, D(14), kWhite, 22, kWhite, 36);
}

void buildMusicBase(CoverJob& j) {
  auto& s = s_music_base;
  const auto c = cv(s);
  int cw = j.cw, ch = j.ch;
  bool have_cover = false;
  // Shades: top for the pills, bottom for the text and controls; applied before packing.
  paint::Shade shades[2] = {{0, D(90), kInk, 140, 0}, {kH - D(300), kH, kInk, 0, 150}};
  if (j.jpg != nullptr) {
    if (cw <= 0 || ch <= 0 || cw > kW || ch > kH) cw = kW, ch = kH;  // older helper: whole panel
    // The cover in the middle, at its own size; its edges fade into the dark page.
    const float fx = std::min<float>(D(60), cw / 5.0f), fy = std::min<float>(D(60), ch / 5.0f);
    const float rim = D(150);  // dark band around the cover before the background shows
    LGFX_Sprite pic(&tft);
    pic.setPsram(true);
    pic.setColorDepth(24);  // decoded at 24 bits, faded and shaded, then dithered: no steps
    if (pic.createSprite(cw, ch) != nullptr) {
      pic.fillScreen(0);
      const uint32_t t0 = micros();
      have_cover = pic.drawJpg(j.jpg, j.len, 0, 0, cw, ch);
      j.dec_us = micros() - t0;
      if (have_cover) {
        shades[1].a1 = 235;
        paint::coverPage(c, static_cast<const uint8_t*>(pic.getBuffer()), coverIsBgr(pic, cw, ch), cw, ch, fx, fy,
                         rim, kCoverBg, shades, 2);
      }
      pic.deleteSprite();
    }
  }
  if (!have_cover) paint::backgroundFast(c, shades, 2);
  // The top bar never changes on this page: part of the base (stage 10c-3).
  pageTabs(c, true);
  radarPill(c);
  if (j.card) nextCardGlass(c);  // 10d-1: ~13 ms a frame less
}

void fmtTime(char* out, size_t cap, uint32_t s) {
  snprintf(out, cap, "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
}

void drawNextCard(const paint::Canvas& c, bool glass);

// Music page parts, in bands that do not overlap (stage 10c-3): text, time bar, then the
// bottom row split into volume, transport and the next card.
constexpr int kMTextY = kTitleY - D(8), kMBarY = kBarY - D(14), kMRowY = kBarY + D(20);
constexpr int kMTransX = kPrevCx - kSmallBtnR - D(2), kMNextX = kNextCx + kSmallBtnR + D(6);
static_assert(kVolX + kVolW + kVolKnobR + D(4) < kMTransX, "volume knob inside its part");
static_assert(kMNextX <= kNextX, "next card inside its part");

template <typename Gfx>
void drawMusicFront(Gfx& s, Frame& f) {
  const paint::Canvas& c = f.c;
  const host::NowPlaying& n = host::now();
  frontPart(kFpTabs);

  if (!n.valid) {
    if (!part(f, 0, host::active(), 0, 0, kW, kH)) return;
    text::drawMid(c, text::Ui22b, "Şu an müzik çalmıyor", kW / 2, kH / 2 - D(16), kText, 255, text::Align::Center);
    text::drawMid(c, text::Ui13,
                  host::active() ? "Bilgisayarda müzik başlayınca burada görünür"
                                 : "Şarkı bilgisi Windows yardımcı programıyla gelir",
                  kW / 2, kH / 2 + D(16), {174, 180, 204}, 255, text::Align::Center);
    text::drawMid(c, text::Ui13, "Sola kaydır: tuşlar", kW - kMusicPad, kCtrlY, kTextMuted, 255, text::Align::Right);
    return;
  }

  const int tx = kMusicPad, tw = kW - 2 * kMusicPad;
  char buf[200];
  if (part(f, 0, mixStr(mixStr(mixStr(kSeed, n.title), n.artist), n.album), 0, kMTextY, kW, kMBarY - kMTextY)) {
    // Title and artist: quiet sizes, the artist line placed by the fonts' own heights.
    text::fit(text::Ui22b, n.title[0] ? n.title : "Bilinmeyen parça", tw, buf, sizeof(buf));
    const int title_base = kTitleY + text::Ui22b.ascent;
    text::draw(c, text::Ui22b, buf, tx, title_base, kText);
    char who[140];
    if (n.artist[0] && n.album[0]) {
      snprintf(who, sizeof(who), "%s · %s", n.artist, n.album);
    } else {
      strlcpy(who, n.artist[0] ? n.artist : n.album, sizeof(who));
    }
    text::fit(text::Ui16, who, tw, buf, sizeof(buf));
    text::draw(c, text::Ui16, buf, tx, title_base + text::Ui22b.descent + D(4) + text::Ui16.ascent, kTextSub);
  }
  frontPart(kFpText);

  // Progress: times either side of a gradient bar.
  const uint32_t pos = host::positionNow();
  s_shown_pos = pos;
  const int bx = tx + kTimeW, bw = tw - 2 * kTimeW;
  const float fr =
      n.duration_s ? std::min(1.0f, static_cast<float>(host::positionNowMs()) / (n.duration_s * 1000.0f)) : 0.0f;
  if (part(f, 1, mix(mix(mix(kSeed, pos), n.duration_s), lroundf(bw * fr * 8)), 0, kMBarY, kW, kMRowY - kMBarY)) {
    char t0[12], t1[12];
    fmtTime(t0, sizeof(t0), pos);
    fmtTime(t1, sizeof(t1), n.duration_s);
    text::drawMid(c, text::Ui13, t0, tx, kBarY + kBarH / 2, {195, 200, 222});
    if (n.duration_s) text::drawMid(c, text::Ui13, t1, tx + tw, kBarY + kBarH / 2, {195, 200, 222}, 255, text::Align::Right);
    frontPart(kFpText);
    paint::roundRect(c, bx, kBarY, bw, kBarH, kBarH / 2, kWhite, 46);
    paint::gradientBar(c, bx, kBarY, std::max(1, static_cast<int>(bw * fr)), kBarH, kPeach, kPink, kLavender);
    paint::circle(c, bx + bw * fr, kBarY + kBarH / 2.0f, D(7), kWhite, 255);
  }
  frontPart(kFpBars);

  if (part(f, 2, n.playing, kMTransX, kMRowY, kMNextX - kMTransX, kH - kMRowY)) {
    drawTransport(s, c, kPrevCx, kPlayCx, kNextCx, kCtrlY, kSmallBtnR, kPlayR);
  }
  frontPart(kFpCtrls);

  // Volume bar, left of the controls.
  const bool muted = mutedNow();
  const int pct = levelPercent();
  s_shown_level = pct;
  s_shown_muted = muted;
  if (part(f, 3, mix(mix(kSeed, pct), muted), 0, kMRowY, kMTransX, kH - kMRowY)) {
    speakerIcon(s, kMusicPad + D(10), kCtrlY, D(11), col565(muted ? kMutedRed : kWhite));
    paint::roundRect(c, kVolX, kCtrlY - kVolH / 2, kVolW, kVolH, kVolH / 2, kWhite, 46);
    const float vx = kVolX + kVolW * (muted ? 0 : pct) / 100.0f;
    if (!muted && pct > 0) {
      paint::gradientBar(c, kVolX, kCtrlY - kVolH / 2, std::max(kVolH, static_cast<int>(vx - kVolX)), kVolH, kPink,
                         kPink, kLavender);
    }
    paint::circle(c, vx, kCtrlY, kVolKnobR + D(4), kLavender, muted ? 0 : 70);
    paint::circle(c, vx, kCtrlY, kVolKnobR, muted ? Rgb{150, 156, 190} : kWhite, 255);
  }
  frontPart(kFpBars);
  const bool next = nextCardShown();
  // A queue picture arriving bumps the version too.
  cardStart();
  if (part(f, 4, next ? mix(kSeed, host::queueVersion()) : 0, kMNextX, kMRowY, kW - kMNextX, kH - kMRowY)) {
    cardPart(kCpCopy);
    if (next) {
      drawNextCard(c, !s_music_card);
    } else {
      text::drawMid(c, text::Ui13, "Sola kaydır: tuşlar", kW - kMusicPad, kCtrlY, kTextMuted, 255, text::Align::Right);
    }
  }
  frontPart(kFpPanel);
}

// ------------------------------------------------------------------ queue
/** Row pictures, decoded once per picture (the helper sends them one by one). */
void refreshThumbs() {
  const int px = host::queueThumbPx();
  for (int i = 0; i < host::kQueueMax; ++i) {
    const host::QueueRow& r = host::queueRow(i);
    const uint8_t* src = i < host::queueCount() ? r.art : nullptr;
    if (src == s_thumb_src[i]) continue;
    s_thumb_src[i] = src;
    if (src == nullptr) {
      if (s_thumb[i] != nullptr) heap_caps_free(s_thumb[i]);
      s_thumb[i] = nullptr;
      continue;
    }
    LGFX_Sprite t(&tft);
    t.setPsram(true);
    t.setColorDepth(16);
    if (t.createSprite(px, px) == nullptr) continue;
    t.fillScreen(0);
    const uint32_t t0 = micros();
    const bool ok = t.drawJpg(src, r.art_len, 0, 0, px, px);
    services::perf::jpeg(true, micros() - t0);
    if (ok) {
      if (s_thumb[i] == nullptr) {
        s_thumb[i] = static_cast<uint16_t*>(heap_caps_malloc(px * px * 2, MALLOC_CAP_SPIRAM));
      }
      if (s_thumb[i] != nullptr) memcpy(s_thumb[i], t.getBuffer(), px * px * 2);
    }
    t.deleteSprite();
  }
}

/** A row picture (or a soft placeholder) with rounded corners. */
void drawThumb(const paint::Canvas& c, int i, int x, int y, int sz) {
  const int px = host::queueThumbPx();
  const uint16_t* pic = (i >= 0 && i < host::kQueueMax) ? s_thumb[i] : nullptr;
  if (pic == nullptr || sz != px) {
    paint::roundRect(c, x, y, sz, sz, D(7), {120, 112, 190}, 70);
    return;
  }
  const float r = D(7);
  for (int yy = 0; yy < px; ++yy) {
    const int cy = y + yy;
    if (cy < 0 || cy >= kH) continue;
    for (int xx = 0; xx < px; ++xx) {
      const int cx = x + xx;
      if (cx < 0 || cx >= kW) continue;
      // Corner rounding: coverage by the distance to the corner's circle.
      float cov = 1.0f;
      const float qx = xx + 0.5f < r ? r - (xx + 0.5f) : (xx + 0.5f > px - r ? (xx + 0.5f) - (px - r) : 0.0f);
      const float qy = yy + 0.5f < r ? r - (yy + 0.5f) : (yy + 0.5f > px - r ? (yy + 0.5f) - (px - r) : 0.0f);
      if (qx > 0.0f && qy > 0.0f) cov = std::min(std::max(r - sqrtf(qx * qx + qy * qy) + 0.5f, 0.0f), 1.0f);
      if (cov <= 0.0f) continue;
      uint16_t& d = c.px[cy * kW + cx];
      const uint16_t v = pic[yy * px + xx];
      if (cov >= 1.0f) {
        d = v;
        continue;
      }
      int r0, g0, b0, r1, g1, b1;
      paint::unpack(d, &r0, &g0, &b0);
      paint::unpack(v, &r1, &g1, &b1);
      d = paint::pack(static_cast<int>(r0 + (r1 - r0) * cov), static_cast<int>(g0 + (g1 - g0) * cov),
                      static_cast<int>(b0 + (b1 - b0) * cov), cx, cy);
    }
  }
}

/** Bottom right of the music page: the next song; tapped, the queue opens. */
void drawNextCard(const paint::Canvas& c, bool glass) {
  refreshThumbs();
  cardPart(kCpDecode);
  const host::QueueRow& r = host::queueRow(1);
  if (glass) nextCardGlass(c);
  cardPart(kCpGlass);
  const int ts = D(36);
  const int px = host::queueThumbPx();
  if (s_thumb[1] != nullptr && px == ts) {
    drawThumb(c, 1, kNextX + D(8), kNextY + (kNextH - ts) / 2, ts);
  } else {
    paint::roundRect(c, kNextX + D(8), kNextY + (kNextH - ts) / 2, ts, ts, D(6), {120, 112, 190}, 70);
    if (s_thumb[1] != nullptr) {  // the picture is 40 px: its middle 36
      for (int yy = 0; yy < ts; ++yy)
        for (int xx = 0; xx < ts; ++xx)
          c.px[(kNextY + (kNextH - ts) / 2 + yy) * kW + kNextX + D(8) + xx] =
              s_thumb[1][(yy + (px - ts) / 2) * px + xx + (px - ts) / 2];
    }
  }
  cardPart(kCpPics);
  const int tx = kNextX + D(8) + ts + D(10), tw = kNextX + kNextW - D(30) - tx;
  char buf[80];
  text::draw(c, text::Ui13, "SIRADAKİ", tx, kNextY + D(21), {158, 164, 196}, 255, text::Align::Left, 1);
  text::fit(text::Ui13b, r.title, tw, buf, sizeof(buf));
  text::draw(c, text::Ui13b, buf, tx, kNextY + D(39), kText);
  // list icon
  const int ix = kNextX + kNextW - D(22), iy = kNextY + kNextH / 2;
  for (int k = -1; k <= 1; ++k) paint::roundRect(c, ix - D(8), iy + k * D(6) - D(1), k == 1 ? D(10) : D(14), D(2), D(1), kTextSoft, 255);
  cardPart(kCpText);  // with the list icon
}

/** Small cover (cropped square) with its dark rim, for the open queue's left column. */
void buildQueueBase(LGFX_Sprite& s, CoverJob& j) {
  const auto c = cv(s);
  paint::backgroundFast(c);
  int cw = j.cw, ch = j.ch;
  if (j.jpg != nullptr) {
    if (cw <= 0 || ch <= 0 || cw > kW || ch > kH) cw = kW, ch = kH;
    // Decoded straight at its size here, cropped to a square: ~73 KB instead of the whole
    // cover (~691 KB for 480 x 480), which left the queue page short of PSRAM (stage 10b-3).
    const float k = std::max(static_cast<float>(kQCoverS) / cw, static_cast<float>(kQCoverS) / ch);
    const float ox = (cw * k - kQCoverS) / 2.0f, oy = (ch * k - kQCoverS) / 2.0f;
    LGFX_Sprite pic(&tft);
    pic.setPsram(true);
    pic.setColorDepth(24);
    if (pic.createSprite(kQCoverS, kQCoverS) != nullptr) {
      pic.fillScreen(0);
      const uint32_t t0 = micros();
      const bool ok = pic.drawJpg(j.jpg, j.len, -lroundf(ox), -lroundf(oy), 0, 0, 0, 0, k, k);
      j.dec_us = micros() - t0;
      if (ok) {
        const auto* pix = static_cast<const uint8_t*>(pic.getBuffer());
        const bool bgr = coverIsBgr(pic, kQCoverS, kQCoverS);
        const float rim = D(60);
        for (int y = std::max(0, kQCoverY - static_cast<int>(rim)); y < std::min(kH, kQCoverY + kQCoverS + static_cast<int>(rim)); ++y) {
          const float* bg = paint::bgRow(kW, kH, y);
          for (int x = std::max(0, kQCoverX - static_cast<int>(rim)); x < kQCoverX + kQCoverS + static_cast<int>(rim); ++x) {
            const int lx = x - kQCoverX, ly = y - kQCoverY;
            const float ex = std::max({0.0f, static_cast<float>(-lx), static_cast<float>(lx - (kQCoverS - 1))});
            const float ey = std::max({0.0f, static_cast<float>(-ly), static_cast<float>(ly - (kQCoverS - 1))});
            const float near = 1.0f - smooth01(sqrtf(ex * ex + ey * ey) / rim);
            float r = bg[x * 3], g = bg[x * 3 + 1], b = bg[x * 3 + 2];
            const bool inside = lx >= 0 && ly >= 0 && lx < kQCoverS && ly < kQCoverS;
            if (!inside) {
              const float dk = near * 0.8f;
              r += (kCoverBg.r - r) * dk, g += (kCoverBg.g - g) * dk, b += (kCoverBg.b - b) * dk;
            } else {
              const uint8_t* p = pix + (ly * kQCoverS + lx) * 3;
              r = p[bgr ? 2 : 0], g = p[1], b = p[bgr ? 0 : 2];
            }
            c.px[y * kW + x] = paint::pack(static_cast<int>(r + 0.5f), static_cast<int>(g + 0.5f),
                                           static_cast<int>(b + 0.5f), x, y);
          }
        }
      }
      pic.deleteSprite();
    }
  }
  paint::roundRect(c, kQPanelX, kQPanelY, kQPanelW, kQPanelH, D(18), kWhite, 16, kWhite, 34);
  paint::roundRect(c, kQPanelX + D(1), kQPanelY + kQHeadH, kQPanelW - D(2), 1, 0, kWhite, 20);
  pageTabs(c, true);
  radarPill(c);
}

void runJob(CoverJob& j) {
  const uint32_t t0 = micros();
  j.dec_us = 0;
  if (j.queue) {
    buildQueueBase(s_music_base, j);
  } else {
    buildMusicBase(j);
  }
  j.total_us = micros() - t0;
}

void coverTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    runJob(s_job);
    s_job_state.store(Job::Done);
  }
}

/** Back on core 1: what the page was built for, and the timings. */
void finishJob() {
  const CoverJob& j = s_job;
  if (j.jpg != nullptr) host::holdCover(false);
  services::perf::decode(j.dec_us);
  services::perf::jpeg(false, j.total_us);  // the cover decoded and faded in
  ++s_base_serial;
  if (j.queue) {
    ++s_qbase_builds;
    s_qbase_cover_gen = j.gen;
    s_qbase_id = j.id;
    s_qbase_dirty = false;
    s_music_dirty = true;  // its screen was borrowed
  } else {
    s_cover_gen = j.gen;
    s_cover_for_id = j.id;
    s_music_card = j.card;
    s_music_dirty = false;
    s_qbase_dirty = true;  // the open queue is drawn on this screen too
  }
  s_job_state.store(Job::Idle);
}

/** A page base for what plays now: on core 0, or here when its task could not start. */
void startJob(bool queue) {
  size_t len = 0;
  int cw = 0, ch = 0;
  const uint8_t* jpg = host::cover(&len, &cw, &ch);
  s_job = CoverJob{queue, coverForNow() ? jpg : nullptr, len, cw, ch, host::coverGeneration(), host::now().id, 0, 0,
                     !queue && nextCardShown()};
  if (s_job.jpg != nullptr) host::holdCover(true);
  s_job_state.store(Job::Running);
  if (s_job_task != nullptr) {
    xTaskNotifyGive(s_job_task);
    return;
  }
  runJob(s_job);
  finishJob();
}

template <typename Gfx>
void drawQueueRows(Gfx& g, const paint::Canvas& c, int count);

// Open queue parts: the left column in three bands, the panel on the right (stage 10c-3).
constexpr int kQTextY = D(254), kQBandY = kQBarY - D(12), kQRowY = kQBarY + D(18), kQLeftPartW = kQPanelX - D(4);
static_assert(kQCoverX + kQLeftW <= kQLeftPartW, "left column inside its parts");

template <typename Gfx>
void drawQueueFront(Gfx& g, Frame& f) {
  const paint::Canvas& c = f.c;
  const host::NowPlaying& n = host::now();
  frontPart(kFpTabs);
  cardStart();
  refreshThumbs();
  cardPart(kCpDecode);
  frontPart(kFpPanel);
  s_shown_qver = host::queueVersion();
  char buf[200];
  // Left: what plays, a little smaller.
  const int tx = kQCoverX;
  const char* who = n.artist[0] ? n.artist : n.album;
  if (part(f, 0, mixStr(mixStr(kSeed, n.title), who), 0, kQTextY, kQLeftPartW, kQBandY - kQTextY)) {
    text::fit(text::Ui18b, n.title[0] ? n.title : "Bilinmeyen parça", kQLeftW, buf, sizeof(buf));
    const int tb = D(262) + text::Ui18b.ascent;
    text::draw(c, text::Ui18b, buf, tx, tb, kText);
    text::fit(text::Ui13, who, kQLeftW, buf, sizeof(buf));
    text::draw(c, text::Ui13, buf, tx, tb + text::Ui18b.descent + D(4) + text::Ui13.ascent, kTextSub);
  }
  const uint32_t pos = host::positionNow();
  s_shown_pos = pos;
  const int bx = tx + kTimeW, bw = kQLeftW - 2 * kTimeW;
  const float fr =
      n.duration_s ? std::min(1.0f, static_cast<float>(host::positionNowMs()) / (n.duration_s * 1000.0f)) : 0.0f;
  if (part(f, 1, mix(mix(mix(kSeed, pos), n.duration_s), lroundf(bw * fr * 8)), 0, kQBandY, kQLeftPartW,
           kQRowY - kQBandY)) {
    char t0[12], t1[12];
    fmtTime(t0, sizeof(t0), pos);
    fmtTime(t1, sizeof(t1), n.duration_s);
    text::drawMid(c, text::Ui13, t0, tx, kQBarY + kBarH / 2, {195, 200, 222});
    if (n.duration_s) text::drawMid(c, text::Ui13, t1, tx + kQLeftW, kQBarY + kBarH / 2, {195, 200, 222}, 255, text::Align::Right);
    frontPart(kFpText);
    paint::roundRect(c, bx, kQBarY, bw, kBarH, kBarH / 2, kWhite, 46);
    paint::gradientBar(c, bx, kQBarY, std::max(1, static_cast<int>(bw * fr)), kBarH, kPeach, kPink, kLavender);
    paint::circle(c, bx + bw * fr, kQBarY + kBarH / 2.0f, D(7), kWhite, 255);
  }
  frontPart(kFpBars);
  if (part(f, 2, n.valid && n.playing, 0, kQRowY, kQLeftPartW, kH - kQRowY)) {
    drawTransport(g, c, kQPrevCx, kQPlayCx, kQNextCx, kQCtrlY, kQSmallR, kQPlayR);
  }
  frontPart(kFpCtrls);

  // Right: the queue, from the kept panel when nothing in it changed.
  const int count = host::queueCount();
  s_qscroll = std::max(0, std::min(s_qscroll, std::max(0, count - kQRows)));
  cardStart();
  const PanelKey key{host::queueVersion(), s_qbase_builds, s_qscroll, s_qflash};
  const uint32_t pkey = mix(mix(mix(mix(kSeed, key.ver), key.builds), key.scroll), key.flash);
  if (!f.full && f.fb->part[3] == pkey) {  // this buffer already shows it
    services::perf::queuePanel(true);
    frontPart(kFpPanel);
    return;
  }
  if (f.fb != nullptr) f.fb->part[3] = pkey;
  if (s_qpanel.getBuffer() == nullptr) {
    s_qpanel.setPsram(true);
    s_qpanel.setColorDepth(16);
    s_qpanel.createSprite(kQPanelW, kQPanelH);  // no room: drawn every frame as before
  }
  auto* kept = static_cast<uint16_t*>(s_qpanel.getBuffer());
  if (kept != nullptr && key == s_qpanel_key) {
    for (int y = 0; y < kQPanelH; ++y) {
      memcpy(c.px + (kQPanelY + y) * kW + kQPanelX, kept + y * kQPanelW, kQPanelW * 2);
    }
    cardPart(kCpCopy);
    services::perf::queuePanel(true);
    frontPart(kFpPanel);
    return;
  }
  services::perf::queuePanel(false);
  if (!f.full) paint::copyRect(c, f.base, kQPanelX, kQPanelY, kQPanelW, kQPanelH);
  cardPart(kCpCopy);
  drawQueueRows(g, c, count);
  if (kept != nullptr) {
    for (int y = 0; y < kQPanelH; ++y) {
      memcpy(kept + y * kQPanelW, c.px + (kQPanelY + y) * kW + kQPanelX, kQPanelW * 2);
    }
    s_qpanel_key = key;
  }
  cardPart(kCpCopy);
  frontPart(kFpPanel);
}

/** The queue panel's rows on the right (count rows in the list). */
template <typename Gfx>
void drawQueueRows(Gfx& g, const paint::Canvas& c, int count) {
  char buf[200];
  text::draw(c, text::Ui15b, "Sıradakiler", kQPanelX + D(16), kQPanelY + D(24), kText);
  snprintf(buf, sizeof(buf), "%s · %d parça", host::queueSource()[0] ? host::queueSource() : "Sıra", count);
  text::draw(c, text::Ui13, buf, kQPanelX + D(16), kQPanelY + D(41), {158, 164, 196});
  for (int k = -1; k <= 1; k += 2) {  // close cross
    g.drawWideLine(kQCloseCx - D(7), kQCloseCy - k * D(7), kQCloseCx + D(7), kQCloseCy + k * D(7), D(1) + 1,
                   col565({226, 229, 245}));
  }
  cardPart(kCpText);
  const int ts = host::queueThumbPx();
  for (int v = 0; v < kQRows; ++v) {
    const int i = s_qscroll + v;
    if (i >= count) break;
    const host::QueueRow& r = host::queueRow(i);
    const int y = kQPanelY + kQHeadH + v * kQRowH;
    const bool cur = i == 0;
    if (cur || i == s_qflash) {
      paint::roundRect(c, kQPanelX + D(5), y + D(3), kQPanelW - D(10), kQRowH - D(6), D(11), kLavender,
                       i == s_qflash ? 70 : 40);
    } else if (v > 0) {
      paint::roundRect(c, kQPanelX + D(14), y, kQPanelW - D(28), 1, 0, kWhite, 14);
    }
    cardPart(kCpGlass);
    drawThumb(c, i, kQPanelX + D(12), y + (kQRowH - ts) / 2, ts);
    if (cur) {  // playing: three bars over its picture
      const int bx0 = kQPanelX + D(12) + ts / 2 - D(8), by = y + kQRowH / 2 + D(8);
      const int hs[3] = {D(10), D(16), D(8)};
      for (int q = 0; q < 3; ++q) paint::roundRect(c, bx0 + q * D(6), by - hs[q], D(4), hs[q], D(1), kWhite, 235);
    }
    cardPart(kCpPics);
    const int lx = kQPanelX + D(12) + ts + D(12);
    const int dw = text::width(text::Ui13, r.dur);
    const int lw = kQPanelX + kQPanelW - D(14) - dw - D(10) - lx;
    text::fit(cur ? text::Ui13b : text::Ui13, r.title, lw, buf, sizeof(buf));
    text::draw(c, cur ? text::Ui13b : text::Ui13, buf, lx, y + D(26), cur ? kWhite : Rgb{231, 233, 246});
    text::fit(text::Ui13, r.artist, lw, buf, sizeof(buf));
    text::draw(c, text::Ui13, buf, lx, y + D(44), {158, 164, 196});
    text::drawMid(c, text::Ui13, r.dur, kQPanelX + kQPanelW - D(14), y + kQRowH / 2, {158, 164, 196}, 255,
                  text::Align::Right);
    cardPart(kCpText);
  }
  if (count > kQRows) {  // where in the list
    const int track = kQPanelH - kQHeadH - D(16), th = std::max(D(24), track * kQRows / count);
    const int ty = kQPanelY + kQHeadH + D(8) + (track - th) * s_qscroll / std::max(1, count - kQRows);
    paint::roundRect(c, kQPanelX + kQPanelW - D(6), ty, D(3), th, D(1), kWhite, 70);
  }
  cardPart(kCpGlass);
}

void ensureBuffers() {
  if (s_ready) return;
  for (LGFX_Sprite* s : {&s_keys_base, &s_music_base}) {
    if (s->getBuffer() != nullptr) continue;
    s->setPsram(true);
    s->setColorDepth(16);
    if (s->createSprite(kW, kH) == nullptr) {
      Console.println("deck: not enough PSRAM for the screens");
      return;
    }
  }
  buildRing();
  // The page background on a coarse grid, worked out once (~74 KB) and kept: every page is
  // put together from it afterwards.
  if (!paint::bgCacheInit(kW, kH, [](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); })) {
    Console.println("deck: no PSRAM for the background copy, pages drawn the slow way");
  }
  // Below the cable and the screen's own work there; JPEG decoding needs a few KB of stack.
  if (xTaskCreatePinnedToCore(coverTask, "cover", 12288, nullptr, 1, &s_job_task, 0) != pdPASS) {
    s_job_task = nullptr;
    Console.println("deck: no cover task, covers built in the loop");
  }
  s_ready = true;
}

// Keys page parts: the knob (its end dot reaches past the ring) and the transport under it.
constexpr int kKnobM = D(12);
constexpr int kKTransX = kKPrevCx - kKSmallR - D(6), kKTransY = kKCtrlY - kKPlayR - D(6);
static_assert(kKnobY + kKnobD + kKnobM <= kKTransY, "knob and transport parts apart");

/** The moving parts on top of a page's base, into canvas c (drawn on with g); fb is what
 *  that panel buffer holds (nullptr: unknown, everything drawn). */
template <typename Gfx>
void composeInto(Gfx& g, const paint::Canvas& c, FbState* fb) {
  const bool queue = s_page == Page::Music && s_queue_open && host::queueValid();
  const int view = s_page == Page::Keys ? 0 : queue ? 1 : host::now().valid ? 2 : 3;
  LGFX_Sprite& bs = view == 0 ? s_keys_base : s_music_base;
  const uint32_t base = s_base_serial * 4 + view + 1;
  Frame f{c, cv(bs), fb, fb == nullptr || fb->base != base};
  if (f.full) {
    memcpy(c.px, bs.getBuffer(), kW * kH * 2);
    if (fb != nullptr) fb->base = base;
  }
  s_base_done_us = s_front_last_us = micros();
  if (view == 0) {
    // The pressed key: the buffer's old highlight back to the base, the new one on top.
    const int was = f.full ? -1 : static_cast<int>(fb->part[0]) - 1;
    if (f.full || was != s_pressed) {
      if (was >= 0) {
        const Box& b = keyBox(was);
        paint::copyRect(c, f.base, b.x - 1, b.y - 1, b.w + 2, b.h + 2);
      }
      if (s_pressed >= 0) {
        const Box& b = keyBox(s_pressed);
        paint::roundRect(c, b.x, b.y, b.w, b.h, D(18), kLavender, 60, {214, 202, 255}, 150);
      }
      if (fb != nullptr) fb->part[0] = s_pressed + 1;
    }
    const bool muted = mutedNow();
    const int pct = levelPercent();
    if (part(f, 1, mix(mix(kSeed, pct), muted), kKnobX - kKnobM, kKnobY - kKnobM, kKnobD + 2 * kKnobM,
             kKnobD + 2 * kKnobM)) {
      drawKnobLevel(g, c, pct, muted);
    }
    const auto& n = host::now();
    if (part(f, 2, n.valid && n.playing, kKTransX, kKTransY, kKNextCx + kKSmallR + D(6) - kKTransX,
             2 * (kKCtrlY - kKTransY))) {
      drawTransport(g, c, kKPrevCx, kKPlayCx, kKNextCx, kKCtrlY, kKSmallR, kKPlayR);
    }
    s_shown_level = pct;
    s_shown_muted = muted;
  } else if (queue) {
    drawQueueFront(g, f);
  } else {
    drawMusicFront(g, f);
  }
}

void openQueue(bool open) {
  if (open == s_queue_open) return;
  s_queue_open = open;
  s_qscroll = 0;
  s_qflash = -1;
  s_qpick[0] = '\0';
  s_qbase_dirty = true;
  if (!open) s_music_dirty = true;  // its screen was borrowed
}

/** Put the current page together and show it. */
void present() {
  if (!s_ready) return;
  if (s_job_state.load() == Job::Done) finishJob();
  // While a cover build runs, s_dirty stays set: shown once it is done.
  const bool building = s_job_state.load() != Job::Idle;
  if (s_page == Page::Keys) {
    if (s_keys_dirty || s_keys_gen != store::generation()) {
      if (building) return;  // its background shares paint's row buffer with the build
      buildKeysBase();
    }
  } else if (building) {
    return;  // the music base is being written: the last frame stays
  } else if (s_queue_open && host::queueValid()) {
    if (s_qbase_dirty || s_qbase_cover_gen != host::coverGeneration() || s_qbase_id != host::now().id) {
      startJob(true);
      if (s_job_state.load() != Job::Idle) return;
    }
  } else if (s_music_dirty || s_cover_gen != host::coverGeneration() || s_cover_for_id != host::now().id ||
             (s_music_card && !nextCardShown())) {  // its glass under nothing: built again
    startJob(false);
    if (s_job_state.load() != Job::Idle) return;
  }
  const uint32_t t0 = micros();
  uint32_t t_front = 0;
  for (uint32_t& us : s_front_us) us = 0;
  for (uint32_t& us : s_card_us) us = 0;
  if (tft.getRotation() != 2) {
    // Straight into the hidden frame buffer (same pixel format), then shown.
    tft.beginHiddenFrame();
    composeInto(tft, paint::Canvas{tft.hiddenBuffer(), kW, kH}, &s_fb[tft.hiddenIndex()]);
    t_front = micros();
    tft.presentHiddenFrame();
  } else {
    // Screen mounted upside down: put together upright, copy turned.
    if (s_canvas.getBuffer() == nullptr) {
      s_canvas.setPsram(true);
      s_canvas.setColorDepth(16);
      if (s_canvas.createSprite(kW, kH) == nullptr) return;
    }
    composeInto(s_canvas, cv(s_canvas), nullptr);
    for (auto& b : s_fb) b.base = 0;  // the buffers get whole frames here
    t_front = micros();
    tft.beginHiddenFrame();
    uint16_t* fb = tft.hiddenBuffer();
    const auto* src = static_cast<const uint16_t*>(s_canvas.getBuffer());
    for (int i = 0, n = kW * kH; i < n; ++i) fb[n - 1 - i] = src[i];
    tft.presentHiddenFrame();
  }
  services::perf::draw(s_base_done_us - t0, t_front - s_base_done_us, micros() - t_front,
                       s_page == Page::Music && s_queue_open && host::queueValid(), s_front_us, s_card_us);
  s_dirty = false;
  s_last_present_ms = millis();
}

/** Show soon, at most ~30 frames a second (a knob turn can ask for many). */
void presentSoon() {
  s_dirty = true;
  if (millis() - s_last_present_ms >= 33) present();
}

void setPage(Page p) {
  if (p == s_page) return;
  s_page = p;
  s_pressed = -1;
  s_flash = false;
  present();
}

// ------------------------------------------------------------------ actions
void fireKey(int i) {
  const store::Slot& k = store::slot(i);
  Console.printf("Deck: %s\n", k.label);
  if (k.kind == store::Kind::Host) {
    host::sendKey(i);
    return;
  }
  if (!canSendHid()) return;
  if (k.kind == store::Kind::Media) {
    services::hid::sendMedia(static_cast<Media>(k.code));
  } else if (k.kind == store::Kind::Key && k.code != 0) {
    services::hid::sendCombo(k.mods, k.code);
  }
}

void media(Media m) {
  if (canSendHid()) services::hid::sendMedia(m);
}

float angleAt(int x, int y) {
  return atan2f(static_cast<float>(x - kKnobCx), static_cast<float>(kKnobCy - y)) * 180.0f /
         static_cast<float>(M_PI);
}

}  // namespace

const char* pageName() { return s_page == Page::Music ? "music" : "keys"; }

bool musicShown() { return s_page == Page::Music; }

void enter() {
  if (s_level < 0) loadLevel();
  ensureBuffers();
  s_pressed = -1;
  s_flash = false;
  s_down = false;
  s_target = Target::None;
  s_keys_dirty = true;  // USB mode or keys may have changed meanwhile
  for (auto& b : s_fb) b.base = 0;  // other screens drew into the panel buffers
  const auto& n = host::now();
  s_page = (n.valid && n.playing) ? Page::Music : Page::Keys;
  s_was_playing = n.valid && n.playing;
  s_last_id = n.id;
  s_now_gen = host::nowGeneration();
  if (!s_ready) {
    tft.fillScreen(TFT_BLACK);
    return;
  }
  present();
}

bool tick() {
  if (!s_ready) return true;
  const unsigned long now = millis();
  if (s_flash && s_pressed >= 0 && now - s_pressed_ms >= 140) {
    s_pressed = -1;
    s_flash = false;
    s_dirty = true;
  }
  if (s_level_dirty && now - s_level_changed_ms >= 3000) saveLevel();
  if (s_target_level >= 0 && host::active() && now - s_last_send_ms >= 40) {
    host::sendVolume(s_target_level);
    s_last_send_ms = now;
    s_target_level = -1;
  }

  // What is playing: to the music page when music starts, back to the keys after a stop.
  const auto& n = host::now();
  if (s_now_gen != host::nowGeneration()) {
    s_now_gen = host::nowGeneration();
    const bool playing = n.valid && n.playing;
    if (playing && (!s_was_playing || n.id != s_last_id) && s_page == Page::Keys && !s_down) {
      s_page = Page::Music;
    }
    if (!playing && s_was_playing) s_stopped_ms = now;
    if (playing) s_stopped_ms = 0;
    s_was_playing = playing;
    s_last_id = n.id;
    s_dirty = true;
  }
  if (s_page == Page::Music && !(n.valid && n.playing) && s_stopped_ms != 0 && n.back_s >= 0 &&
      now - s_stopped_ms >= static_cast<unsigned long>(n.back_s) * 1000UL && !s_down) {
    s_stopped_ms = 0;
    s_page = Page::Keys;
    s_dirty = true;
  }
  if (s_queue_open && !host::queueValid()) s_queue_open = false;  // the queue went away
  if (s_qflash >= 0 && now - s_qflash_ms >= 400) {
    s_qflash = -1;
    s_dirty = true;
  }
  {
    static uint32_t last_qver = 0;
    if (host::queueVersion() != last_qver) {
      last_qver = host::queueVersion();
      if (s_page == Page::Music) s_dirty = true;
      if (s_qpick[0] != '\0' && host::queueCount() > 0 &&
          strcmp(host::queueRow(0).title, s_qpick) == 0) {
        s_qscroll = 0;
        s_qpick[0] = '\0';
      }
    }
  }
  if (s_qpick[0] != '\0' && now - s_qpick_ms >= 5000) s_qpick[0] = '\0';
  if (s_page == Page::Music) {
    // The base the page shows: the open queue keeps its own (stage 10c: checking the music
    // base there redrew every turn after a cover arrived, ~11 frames a second).
    const bool q = s_queue_open && host::queueValid();
    if ((q ? s_qbase_cover_gen : s_cover_gen) != host::coverGeneration() || (q ? s_qbase_id : s_cover_for_id) != n.id) {
      s_dirty = true;
    }
    if (n.valid && host::positionNow() != s_shown_pos) s_dirty = true;
  }
  if (store::generation() != s_keys_gen && s_page == Page::Keys) s_dirty = true;
  if (levelPercent() != s_shown_level || mutedNow() != s_shown_muted) s_dirty = true;
  if (s_dirty) present();
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
      s_key = -1;
      if (inRadar(x, y)) {
        s_target = Target::Back;
      } else if (tabAt(x, y) != 0) {
        s_target = Target::Tab;
      } else if (s_page == Page::Keys) {
        if (inCircle(x, y, kKPrevCx, kKCtrlY, kKSmallR + D(10))) {
          s_target = Target::Prev;
        } else if (inCircle(x, y, kKPlayCx, kKCtrlY, kKPlayR + D(10))) {
          s_target = Target::Play;
        } else if (inCircle(x, y, kKNextCx, kKCtrlY, kKSmallR + D(10))) {
          s_target = Target::Next;
        } else if (inKnob(x, y)) {
          if (millis() >= s_user_until_ms) s_user_level = host::active() ? host::volume() : -1;
          s_target = Target::Knob;
          s_last_deg = angleAt(x, y);
          s_accum_deg = s_total_deg = 0;
        } else if ((s_key = keyAt(x, y)) >= 0) {
          s_target = Target::Key;
          s_pressed = s_key;
          s_flash = false;
          presentSoon();
        } else {
          s_target = Target::None;
        }
      } else if (host::now().valid && s_queue_open && host::queueValid()) {
        if (inCircle(x, y, kQCloseCx, kQCloseCy, D(26))) {
          s_target = Target::QClose;
        } else if (x >= kQPanelX && x < kQPanelX + kQPanelW && y >= kQPanelY + kQHeadH && y < kQPanelY + kQPanelH) {
          s_target = Target::QList;
          s_qscroll_down = s_qscroll;
        } else if (inCircle(x, y, kQPrevCx, kQCtrlY, kQSmallR + D(10))) {
          s_target = Target::Prev;
        } else if (inCircle(x, y, kQPlayCx, kQCtrlY, kQPlayR + D(10))) {
          s_target = Target::Play;
        } else if (inCircle(x, y, kQNextCx, kQCtrlY, kQSmallR + D(10))) {
          s_target = Target::Next;
        } else {
          s_target = Target::None;
        }
      } else if (host::now().valid) {
        if (host::queueValid() && host::queueCount() > 1 && x >= kNextX - D(6) && x < kNextX + kNextW + D(6) &&
            y >= kNextY - D(8) && y < kNextY + kNextH + D(8)) {
          s_target = Target::QOpen;
        } else if (inCircle(x, y, kPrevCx, kCtrlY, kSmallBtnR + D(8))) {
          s_target = Target::Prev;
        } else if (inCircle(x, y, kPlayCx, kCtrlY, kPlayR + D(8))) {
          s_target = Target::Play;
        } else if (inCircle(x, y, kNextCx, kCtrlY, kSmallBtnR + D(8))) {
          s_target = Target::Next;
        } else if (x >= D(8) && x < kVolX + kVolW + D(16) && abs(y - kCtrlY) < kVolTouchH) {
          if (millis() >= s_user_until_ms) s_user_level = -1;
          s_target = Target::VolBar;
          s_bar_x = x;
          wantLevel(static_cast<int>(lroundf((s_bar_x - kVolX) * 100.0f / kVolW)));
          presentSoon();
        } else {
          s_target = Target::None;
        }
      } else {
        s_target = Target::None;
      }
      return Touch::None;
    }
    const int dx = x - s_down_x, dy = y - s_down_y;
    const int move = abs(dx) + abs(dy);
    if (move > s_max_move) s_max_move = move;
    if (s_target == Target::QList && abs(dy) >= abs(dx)) {  // the list follows the finger, row by row
      const int rows = std::max(0, host::queueCount() - kQRows);
      const int want = std::max(0, std::min(rows, s_qscroll_down - dy / kQRowH));
      if (want != s_qscroll) {
        s_qscroll = want;
        presentSoon();
      }
      return Touch::None;
    }
    // A sideways slide anywhere but on the knob / volume bar turns the page; it starts
    // early, so a quick flick over a key is a page turn and not a key press.
    if (s_target != Target::Knob && s_target != Target::VolBar && s_target != Target::Swipe &&
        abs(dx) > D(24) && 2 * abs(dx) > 3 * abs(dy)) {
      if (s_target == Target::Key) {
        s_pressed = -1;
        presentSoon();
      }
      s_target = Target::Swipe;
    }
    if (s_target == Target::Knob) {
      const int kx = x - kKnobCx, ky = y - kKnobCy;
      if (kx * kx + ky * ky < (kKnobD / 8) * (kKnobD / 8)) return Touch::None;  // centre: no angle
      const float deg = angleAt(x, y);
      float d = deg - s_last_deg;
      if (d > 180.0f) d -= 360.0f;
      if (d < -180.0f) d += 360.0f;
      s_last_deg = deg;
      s_accum_deg += d;
      s_total_deg += fabsf(d);
      if (host::active()) {
        // With the helper: a full turn is 0..100 %, set directly.
        const int dp = static_cast<int>(s_accum_deg / 3.6f);
        if (dp != 0) {
          s_accum_deg -= dp * 3.6f;
          wantLevel((userHold() ? s_user_level : host::volume()) + dp);
          presentSoon();
        }
      } else {
        const float step = 360.0f / steps();
        bool changed = false;
        while (s_accum_deg >= step) {
          s_accum_deg -= step;
          hidStep(+1);
          changed = true;
        }
        while (s_accum_deg <= -step) {
          s_accum_deg += step;
          hidStep(-1);
          changed = true;
        }
        if (changed) presentSoon();
      }
    } else if (s_target == Target::VolBar) {
      // The touch panel wobbles by a pixel or two: smooth it, so the level does not flicker.
      s_bar_x += (x - s_bar_x) * 0.5f;
      wantLevel(static_cast<int>(lroundf((s_bar_x - kVolX) * 100.0f / kVolW)));
      presentSoon();
    } else if (s_target == Target::Key && keyAt(x, y) != s_key && s_pressed == s_key) {
      s_pressed = -1;  // slid off the key: no press
      presentSoon();
    }
    return Touch::None;
  }

  if (!s_down) return Touch::None;
  s_down = false;
  x = s_last_x;
  y = s_last_y;
  const Target t = s_target;
  s_target = Target::None;
  {
    // Finger moved away sideways (also a flick too quick to be seen while moving):
    // left = keys, right = music.
    const int dx = x - s_down_x, dy = y - s_down_y;
    const bool flick = abs(dx) >= D(24) && 2 * abs(dx) > 3 * abs(dy);
    if (t == Target::Swipe ||
        (flick && (t == Target::Key || t == Target::None || t == Target::Prev || t == Target::Play ||
                   t == Target::Next || t == Target::Tab || t == Target::QOpen || t == Target::QClose))) {
      if (t == Target::Key) s_pressed = -1;
      if (s_queue_open) {  // a sideways slide first closes the queue
        openQueue(false);
        presentSoon();
        return Touch::None;
      }
      if (dx >= D(24)) {
        setPage(Page::Music);
      } else if (dx <= -D(24)) {
        setPage(Page::Keys);
      } else {
        presentSoon();
      }
      return Touch::None;
    }
  }
  switch (t) {
    case Target::Back:
      return inRadar(x, y) || s_max_move < D(20) ? Touch::Back : Touch::None;
    case Target::Swipe:
      return Touch::None;  // handled above
    case Target::Tab:
      if (s_max_move < D(24)) {
        if (tabAt(s_down_x, s_down_y) != 1) openQueue(false);
        setPage(tabAt(s_down_x, s_down_y) == 1 ? Page::Music : Page::Keys);
      }
      return Touch::None;
    case Target::QOpen:
      if (s_max_move < D(20)) {
        openQueue(true);
        presentSoon();
      }
      return Touch::None;
    case Target::QClose:
      if (s_max_move < D(24)) {
        openQueue(false);
        presentSoon();
      }
      return Touch::None;
    case Target::QList:
      if (s_max_move < D(16)) {
        const int row = s_qscroll + (s_down_y - (kQPanelY + kQHeadH)) / kQRowH;
        if (row > 0 && row < host::queueCount()) {  // the playing one needs nothing
          host::sendQueuePlay(row);
          strlcpy(s_qpick, host::queueRow(row).title, sizeof(s_qpick));
          s_qpick_ms = millis();
          s_qflash = row;
          s_qflash_ms = millis();
          presentSoon();
        }
      }
      return Touch::None;
    case Target::Knob:
      if (s_total_deg < 8.0f && s_max_move < D(14) && now - s_down_ms < 700) {
        toggleMute();
        presentSoon();
      } else if (s_target_level >= 0 && host::active()) {
        host::sendVolume(s_target_level);
        s_target_level = -1;
      }
      if (s_user_level >= 0) s_user_until_ms = millis() + 1200;
      return Touch::None;
    case Target::Key:
      if (s_pressed == s_key && s_max_move < D(16)) {
        fireKey(s_key);
        s_pressed_ms = millis();
        s_flash = true;  // tick() lets go of the highlight
      } else if (s_pressed >= 0) {
        s_pressed = -1;  // moved too much: no press
        presentSoon();
      }
      return Touch::None;
    case Target::Prev:
      if (s_max_move < D(20)) media(Media::Previous);
      return Touch::None;
    case Target::Play:
      if (s_max_move < D(20)) media(Media::PlayPause);
      return Touch::None;
    case Target::Next:
      if (s_max_move < D(20)) media(Media::Next);
      return Touch::None;
    case Target::VolBar:
      if (s_target_level >= 0 && host::active()) {
        host::sendVolume(s_target_level);
        s_target_level = -1;
      }
      s_user_until_ms = millis() + 1200;  // the computer catches up meanwhile
      return Touch::None;
    case Target::None:
      return Touch::None;
  }
  return Touch::None;
}

}  // namespace ui::deck

#endif  // BOARD_WS5
