#include "services/map_store.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_arduino_version.h>
#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <spi_flash_mmap.h>
#define MAP_MMAP_DATA ESP_PARTITION_MMAP_DATA
typedef esp_partition_mmap_handle_t map_mmap_handle_t;
#define map_munmap(h) esp_partition_munmap(h)
#else
#include <esp_spi_flash.h>
#define MAP_MMAP_DATA SPI_FLASH_MMAP_DATA
typedef spi_flash_mmap_handle_t map_mmap_handle_t;
#define map_munmap(h) spi_flash_munmap(h)
#endif

#include <cstdlib>
#include <cstring>

namespace services::map {

namespace {

constexpr uint32_t kMagic = 0x504D5250;  // "PRMP"
constexpr size_t kSector = 4096;

struct FlashHeader {
  uint32_t magic;
  uint32_t length;
  uint32_t crc;
};

// The active map is read straight from flash through the cache (memory-mapped), so it
// costs no RAM: with Wi-Fi on, the HTTPS client needs every free kilobyte.
const uint8_t* s_active = nullptr;
size_t s_active_len = 0;
map_mmap_handle_t s_mmap = 0;
bool s_mapped = false;
uint8_t* s_active_ram = nullptr;  // fallback copy if mapping fails
uint32_t s_generation = 1;

uint8_t* s_stage = nullptr;
size_t s_stage_len = 0;
size_t s_stage_got = 0;
uint32_t s_stage_crc = 0;

const esp_partition_t* partition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
}

void releaseActive() {
  if (s_mapped) {
    map_munmap(s_mmap);
    s_mapped = false;
  }
  free(s_active_ram);
  s_active_ram = nullptr;
  s_active = nullptr;
  s_active_len = 0;
  ++s_generation;
}

uint16_t rd16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

// The blob must be exactly consumed by its polylines; anything else is rejected
// so a damaged upload can never make the radar read outside the buffer.
bool validBlob(const uint8_t* b, size_t n) {
  if (n < kHeaderBytes || n > kMaxBytes || b[0] != 'P' || b[1] != 'M' || b[2] < 1 ||
      b[2] > kBlobVersion) {
    return false;
  }
  const uint16_t count = rd16(b + 12);
  size_t i = kHeaderBytes;
  for (uint16_t k = 0; k < count; ++k) {
    if (i + 3 > n) {
      return false;
    }
    const uint16_t pts = rd16(b + i + 1);
    i += 3 + static_cast<size_t>(pts) * 4;
    if (i > n) {
      return false;
    }
  }
  return i == n;
}

void freeStage() {
  free(s_stage);
  s_stage = nullptr;
  s_stage_len = s_stage_got = 0;
}

}  // namespace

uint32_t crc32(const uint8_t* bytes, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= bytes[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}


/** Map (or, failing that, copy) the stored map; checks magic, CRC and structure. */
bool activateStored() {
  const esp_partition_t* part = partition();
  if (part == nullptr) return false;
  FlashHeader h;
  if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK || h.magic != kMagic ||
      h.length < kHeaderBytes || h.length > kMaxBytes) {
    return false;
  }
  const void* ptr = nullptr;
  map_mmap_handle_t handle;
  if (esp_partition_mmap(part, 0, sizeof(h) + h.length, MAP_MMAP_DATA, &ptr, &handle) ==
      ESP_OK) {
    const uint8_t* blob = static_cast<const uint8_t*>(ptr) + sizeof(h);
    if (crc32(blob, h.length) != h.crc || !validBlob(blob, h.length)) {
      map_munmap(handle);
      Console.println("map: stored map is damaged, ignoring it");
      return false;
    }
    s_mmap = handle;
    s_mapped = true;
    s_active = blob;
  } else {
    uint8_t* buf = static_cast<uint8_t*>(malloc(h.length));
    if (buf == nullptr) {
      Console.println("map: no RAM for stored map");
      return false;
    }
    if (esp_partition_read(part, sizeof(h), buf, h.length) != ESP_OK ||
        crc32(buf, h.length) != h.crc || !validBlob(buf, h.length)) {
      free(buf);
      Console.println("map: stored map is damaged, ignoring it");
      return false;
    }
    s_active_ram = buf;
    s_active = buf;
  }
  s_active_len = h.length;
  ++s_generation;
  Console.printf("map: %u bytes (%s)\n", static_cast<unsigned>(h.length),
                s_mapped ? "read from flash" : "copied to RAM");
  return true;
}

void init() { activateStored(); }

const uint8_t* data() { return s_active; }
size_t size() { return s_active_len; }
uint32_t generation() { return s_generation; }

bool beginStaging(size_t length, uint32_t crc) {
  freeStage();
  if (length < kHeaderBytes || length > kMaxBytes) {
    return false;
  }
  s_stage = static_cast<uint8_t*>(malloc(length));
  if (s_stage == nullptr && s_active_ram != nullptr) {
    // Low on RAM: give up the old map copy (it is replaced anyway) and try again.
    releaseActive();
    s_stage = static_cast<uint8_t*>(malloc(length));
  }
  if (s_stage == nullptr) {
    return false;
  }
  s_stage_len = length;
  s_stage_got = 0;
  s_stage_crc = crc;
  return true;
}

bool stageChunk(const uint8_t* bytes, size_t n) {
  if (s_stage == nullptr || s_stage_got + n > s_stage_len) {
    return false;
  }
  memcpy(s_stage + s_stage_got, bytes, n);
  s_stage_got += n;
  return true;
}

bool commit() {
  if (s_stage == nullptr || s_stage_got != s_stage_len ||
      crc32(s_stage, s_stage_len) != s_stage_crc || !validBlob(s_stage, s_stage_len)) {
    freeStage();
    return false;
  }
  const esp_partition_t* part = partition();
  if (part == nullptr) {
    freeStage();
    return false;
  }
  const FlashHeader h = {kMagic, static_cast<uint32_t>(s_stage_len), s_stage_crc};
  const size_t total = sizeof(h) + s_stage_len;
  const size_t erase_len = ((total + kSector - 1) / kSector) * kSector;
  releaseActive();  // the mapped flash is about to be erased
  const bool ok = esp_partition_erase_range(part, 0, erase_len) == ESP_OK &&
                  esp_partition_write(part, 0, &h, sizeof(h)) == ESP_OK &&
                  esp_partition_write(part, sizeof(h), s_stage, s_stage_len) == ESP_OK;
  freeStage();
  if (!ok) return false;
  return activateStored();
}

void abortStaging() { freeStage(); }

void clear() {
  freeStage();
  releaseActive();
  const esp_partition_t* part = partition();
  if (part != nullptr) {
    esp_partition_erase_range(part, 0, kSector);  // header sector only
  }
}

}  // namespace services::map
