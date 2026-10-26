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
constexpr uint32_t kRegionBytes = 0x40000;  // 256 KB
constexpr size_t kSector = 4096;
constexpr uint32_t kMagic = 0x4B445250;  // "PRDK"
constexpr size_t kHeadBytes = 6;

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
      makeSlot("Ctrl+Alt+Shift+P", Kind::Key, 'p', hid::kCtrl | hid::kAlt | hid::kShift),
  };
  memcpy(s_slots, defaults, sizeof(s_slots));
  for (auto& p : s_icons) p = nullptr;
  s_custom = false;
}

/** Checks a blob and points slots / icons into it. */
bool apply(const uint8_t* b, size_t n) {
  if (n < kHeadBytes + kSlots * sizeof(Slot) || b[0] != 'D' || b[1] != 'K' || b[2] != kVersion ||
      b[3] != kSlots) {
    return false;
  }
  const int px = b[4];
  const size_t icon_bytes = static_cast<size_t>(px) * px * 2;
  Slot slots[kSlots];
  memcpy(slots, b + kHeadBytes, sizeof(slots));
  size_t off = kHeadBytes + sizeof(slots);
  const uint16_t* icons[kSlots] = {};
  for (int i = 0; i < kSlots; ++i) {
    slots[i].label[kLabelBytes - 1] = '\0';
    if (static_cast<uint8_t>(slots[i].kind) > static_cast<uint8_t>(Kind::Key)) slots[i].kind = Kind::Empty;
    if (!slots[i].has_icon) continue;
    if (off + icon_bytes > n) return false;
    // A set made for the other panel size: keys keep their labels, pictures are dropped.
    if (px == iconPx()) icons[i] = reinterpret_cast<const uint16_t*>(b + off);
    off += icon_bytes;
  }
  if (off != n) return false;
  memcpy(s_slots, slots, sizeof(s_slots));
  memcpy(s_icons, icons, sizeof(s_icons));
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
  if (length < kHeadBytes || length > kRegionBytes - sizeof(Header)) return false;
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
