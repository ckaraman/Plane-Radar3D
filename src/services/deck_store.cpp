#include "services/deck_store.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>

#include <cstring>

#include "hardware/console.h"
#include "services/hid_keys.h"
#include "services/map_store.h"
#include "ui/ui_scale.h"

namespace services::deck {

namespace {

// Region of the data ("spiffs") partition: the map blob sits at its start (32 KB max).
constexpr uint32_t kRegionOffset = 0x900000;
constexpr uint32_t kRegionBytes = 0xC0000;  // 768 KB: whole-key pictures on the 5B (12 x 141 x 153 x 2)
constexpr size_t kSector = 4096;
constexpr uint32_t kMagic = 0x4B445250;  // "PRDK"
constexpr size_t kHeadBytes1 = 6, kHeadBytes2 = 8, kHeadBytes3 = 12;

struct Header {
  uint32_t magic;
  uint32_t length;
  uint32_t crc;
};

constexpr uint8_t kF13 = 0xF0;  // Arduino code of F13 (USBHIDKeyboard.h)

Slot makeSlot(const char* label, Kind kind, uint8_t code, uint8_t mods = 0) {
  Slot s{};
  s.kind = kind;
  s.code = code;
  s.mods = mods;
  strlcpy(s.label, label, sizeof(s.label));
  return s;
}

Slot s_slots[kSlots];
const uint16_t* s_icons[kSlots] = {};
uint8_t* s_blob = nullptr;  // PSRAM copy of the stored set (icons point into it)
bool s_custom = false;
bool s_full = false;
bool s_compact = false;
int s_cols = 4, s_rows = 3, s_kw = keyW(), s_kh = keyH();
int s_volume_steps = 16;
uint32_t s_gen = 1;

uint8_t* s_stage = nullptr;
size_t s_stage_len = 0;
size_t s_stage_got = 0;
uint32_t s_stage_crc = 0;

void loadDefaults() {
  const Slot defaults[kSlots] = {
      makeSlot("Önceki", Kind::Media, static_cast<uint8_t>(hid::Media::Previous)),
      makeSlot("Oynat / Dur", Kind::Media, static_cast<uint8_t>(hid::Media::PlayPause)),
      makeSlot("Sonraki", Kind::Media, static_cast<uint8_t>(hid::Media::Next)),
      makeSlot("F13", Kind::Key, kF13 + 0),
      makeSlot("F14", Kind::Key, kF13 + 1),
      makeSlot("F15", Kind::Key, kF13 + 2),
      makeSlot("F16", Kind::Key, kF13 + 3),
      makeSlot("F17", Kind::Key, kF13 + 4),
      makeSlot("F18", Kind::Key, kF13 + 5),
      makeSlot("F19", Kind::Key, kF13 + 6),
      makeSlot("F20", Kind::Key, kF13 + 7),
      makeSlot("Kısayol P", Kind::Key, 'p', hid::kCtrl | hid::kAlt | hid::kShift),
  };
  memcpy(s_slots, defaults, sizeof(s_slots));
  for (auto& p : s_icons) p = nullptr;
  s_custom = false;
  s_full = false;
  s_compact = false;
  s_cols = 4, s_rows = 3, s_kw = keyW(), s_kh = keyH();
  s_volume_steps = 16;
}

/** Checks a blob and points slots / icons into it. */
bool apply(const uint8_t* b, size_t n) {
  if (n < kHeadBytes1 || b[0] != 'D' || b[1] != 'K' || b[2] < 1 || b[2] > 3 || b[3] != kSlots) return false;
  const int ver = b[2];
  const size_t head = ver == 3 ? kHeadBytes3 : (ver == 2 ? kHeadBytes2 : kHeadBytes1);
  if (n < head + kSlots * sizeof(Slot)) return false;
  int iw, ih, steps, cols = 4, rows = 3;
  if (ver == 3) {
    cols = b[4];
    rows = b[5];
    steps = b[6];
    iw = b[8] | (b[9] << 8);
    ih = b[10] | (b[11] << 8);
  } else {
    iw = b[4];
    ih = ver == 2 ? b[5] : b[4];
    steps = ver == 2 ? b[6] : b[5];
  }
  const size_t icon_bytes = static_cast<size_t>(iw) * ih * 2;
  bool fits;
  if (ver == 3) {
    // Made for this panel: the grid fills the key area (a pixel or two of rounding).
    fits = cols >= 1 && rows >= 1 && cols <= kSlots && rows <= kSlots && iw > 0 && ih > 0 &&
           cols * iw + (cols - 1) * keyGap() <= keyAreaW() + 2 && rows * ih + (rows - 1) * keyGap() <= keyAreaH() + 2;
  } else {
    // A set made for the other panel size: keys keep their labels, pictures are dropped.
    fits = ver == 2 ? (iw == keyW() && ih == keyH()) : (iw == iconPx());
  }
  Slot slots[kSlots];
  memcpy(slots, b + head, sizeof(slots));
  size_t off = head + sizeof(slots);
  const uint16_t* icons[kSlots] = {};
  int used = 0;
  for (int i = 0; i < kSlots; ++i) {
    slots[i].label[kLabelBytes - 1] = '\0';
    if (static_cast<uint8_t>(slots[i].kind) > static_cast<uint8_t>(Kind::Host)) slots[i].kind = Kind::Empty;
    if (slots[i].kind != Kind::Empty || slots[i].label[0] || slots[i].has_icon) ++used;
    if (!slots[i].has_icon) continue;
    if (off + icon_bytes > n) return false;
    if (fits) icons[i] = reinterpret_cast<const uint16_t*>(b + off);
    off += icon_bytes;
  }
  if (off != n) return false;
  memcpy(s_slots, slots, sizeof(s_slots));
  memcpy(s_icons, icons, sizeof(s_icons));
  s_volume_steps = (steps >= 4 && steps <= 100) ? steps : 16;
  s_full = ver >= 2;
  s_compact = ver == 3 && fits && used > 0 && cols * rows >= used;
  if (s_compact) {
    s_cols = cols, s_rows = rows, s_kw = iw, s_kh = ih;
  } else {
    s_cols = 4, s_rows = 3, s_kw = keyW(), s_kh = keyH();
    if (ver == 3) s_full = false;  // pictures of another size: not drawn at all
  }
  s_custom = true;
  return true;
}

const esp_partition_t* partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

void freeBlob() {
  if (s_blob != nullptr) heap_caps_free(s_blob);
  s_blob = nullptr;
}

}  // namespace

int iconPx() { return ui::S(48); }
int volumeSteps() { return s_volume_steps; }
bool fullKeyIcons() { return s_full; }
bool compactLayout() { return s_compact; }
int layoutCols() { return s_cols; }
int layoutRows() { return s_rows; }
int layoutKeyW() { return s_kw; }
int layoutKeyH() { return s_kh; }
bool slotUsed(int i) {
  const Slot& k = slot(i);
  return k.kind != Kind::Empty || k.label[0] != '\0' || k.has_icon;
}

void init() {
  loadDefaults();
  const esp_partition_t* part = partition();
  if (part == nullptr || part->size < kRegionOffset + kRegionBytes) return;
  Header h{};
  if (esp_partition_read(part, kRegionOffset, &h, sizeof(h)) != ESP_OK || h.magic != kMagic ||
      h.length == 0 || h.length > kRegionBytes - sizeof(h)) {
    return;
  }
  auto* b = static_cast<uint8_t*>(heap_caps_malloc(h.length, MALLOC_CAP_SPIRAM));
  if (b == nullptr) return;
  if (esp_partition_read(part, kRegionOffset + sizeof(h), b, h.length) != ESP_OK ||
      services::map::crc32(b, h.length) != h.crc || !apply(b, h.length)) {
    heap_caps_free(b);
    loadDefaults();
    Console.println("deck: stored keys unreadable, defaults");
    return;
  }
  s_blob = b;
  Console.printf("deck: %u bytes of keys loaded\n", static_cast<unsigned>(h.length));
}

const Slot& slot(int i) { return s_slots[i < 0 ? 0 : (i >= kSlots ? kSlots - 1 : i)]; }
const uint16_t* icon(int i) { return (i >= 0 && i < kSlots) ? s_icons[i] : nullptr; }
uint32_t generation() { return s_gen; }
bool custom() { return s_custom; }

bool beginStaging(size_t length, uint32_t crc32) {
  abortStaging();
  if (length < kHeadBytes1 || length > kRegionBytes - sizeof(Header)) return false;
  s_stage = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM));
  if (s_stage == nullptr) return false;
  s_stage_len = length;
  s_stage_got = 0;
  s_stage_crc = crc32;
  return true;
}

bool stageChunk(const uint8_t* bytes, size_t n) {
  if (s_stage == nullptr || s_stage_got + n > s_stage_len) return false;
  memcpy(s_stage + s_stage_got, bytes, n);
  s_stage_got += n;
  return true;
}

bool commit() {
  if (s_stage == nullptr || s_stage_got != s_stage_len ||
      services::map::crc32(s_stage, s_stage_len) != s_stage_crc) {
    abortStaging();
    return false;
  }
  // apply() changes the active set only when the blob is valid.
  if (!apply(s_stage, s_stage_len)) {
    abortStaging();
    return false;
  }
  const esp_partition_t* part = partition();
  bool ok = part != nullptr && part->size >= kRegionOffset + kRegionBytes;
  if (ok) {
    const Header h{kMagic, static_cast<uint32_t>(s_stage_len), s_stage_crc};
    const size_t total = sizeof(h) + s_stage_len;
    const size_t erase_len = ((total + kSector - 1) / kSector) * kSector;
    ok = esp_partition_erase_range(part, kRegionOffset, erase_len) == ESP_OK &&
         esp_partition_write(part, kRegionOffset, &h, sizeof(h)) == ESP_OK &&
         esp_partition_write(part, kRegionOffset + sizeof(h), s_stage, s_stage_len) == ESP_OK;
  }
  // The staged buffer becomes the active one (the icons point into it).
  freeBlob();
  s_blob = s_stage;
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
  ++s_gen;
  if (!ok) Console.println("deck: flash write failed (keys active until restart)");
  return true;
}

void abortStaging() {
  if (s_stage != nullptr) heap_caps_free(s_stage);
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
}

void clear() {
  const esp_partition_t* part = partition();
  if (part != nullptr && part->size >= kRegionOffset + kRegionBytes) {
    esp_partition_erase_range(part, kRegionOffset, kSector);
  }
  freeBlob();
  loadDefaults();
  ++s_gen;
}

}  // namespace services::deck
