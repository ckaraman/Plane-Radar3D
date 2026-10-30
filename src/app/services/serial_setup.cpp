#include "services/serial_setup.h"
#include "hardware/console.h"
#if defined(WITH_DECK)
#include "services/deck_store.h"
#include "services/host_link.h"
#include "services/perf.h"
#endif

#include <Arduino.h>
#if defined(WITH_WIFI)
#include <WiFi.h>
#include <esp_wifi.h>
#endif

#include <ArduinoJson.h>
#include <esp_mac.h>
#include <esp_system.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/clock_service.h"
#include "services/weather_service.h"
#include "services/map_store.h"
#include "services/radar_location.h"
#include "services/usb_feed.h"
#include "services/serial_ota.h"
#include "services/terrain.h"
#if defined(WITH_WIFI)
#include "services/wifi_setup.h"
#endif
#include "ui/radar_range.h"

namespace services::serial_setup {

namespace {

constexpr size_t kMaxLine = 700;
constexpr size_t kMaxReply = 2048;
constexpr size_t kTzMaxLen = 47;
constexpr unsigned long kJoinTimeoutMs = 20000;
constexpr unsigned long kNotFoundGiveUpMs = 8000;
constexpr size_t kMaxScanResults = 20;

char s_line[kMaxLine];
size_t s_len = 0;
bool s_overflow = false;

// One write() per line keeps a reply from being split by the log output of
// the ADS-B task, which prints from the other core.
void sendReply(const char* tag, const JsonDocument& doc) {
  // Allocated per reply: the setup page talks rarely, so no static 2 KB buffer.
  const size_t body_len = measureJson(doc);
  const size_t cap = strlen(tag) + body_len + 8;
  if (cap > kMaxReply) return;
  char* reply = static_cast<char*>(malloc(cap));
  if (reply == nullptr) return;
  int n = snprintf(reply, cap, "PR:%s ", tag);
  if (n < 0) {
    free(reply);
    return;
  }
  const size_t body = serializeJson(doc, reply + n, cap - static_cast<size_t>(n) - 2);
  size_t total = static_cast<size_t>(n) + body;
  reply[total++] = '\n';
  Console.write(reinterpret_cast<const uint8_t*>(reply), total);
  free(reply);
}

void sendError(const char* code) {
  JsonDocument doc;
  doc["msg"] = code;
  sendReply("ERR", doc);
}

void sendOk(bool rebooting, const char* ip = nullptr) {
  JsonDocument doc;
  doc["reboot"] = rebooting;
  if (ip != nullptr) {
    doc["ip"] = ip;
  }
  sendReply("OK", doc);
}

#if !defined(WITH_WIFI)
/** Screen settings the PC needs (PR:PREFS, docs/PROTOCOL.md): it filters the aircraft by them. */
void fillPrefs(JsonObject p) {
  p["range_km"] = ui::radar::rangeCurrent().outer_km;
  p["fetch_km"] = ui::radar::fetchRadiusKm();
  p["miles"] = ui::radar::useMiles();
}
#endif

void sendHello() {
  JsonDocument doc;
  doc["fw"] = config::kFirmwareVersion;
  doc["board"] = "esp32-3d";
#if defined(BOARD_WS5B)
  doc["hw"] = "ws5b";  // the setup page sends firmware-ws5b.bin for USB updates
#elif defined(BOARD_WS5)
  doc["hw"] = "ws5";  // the setup page sends firmware-ws5.bin for USB updates
#else
  doc["hw"] = "wroom";
#endif

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // the same id with or without Wi-Fi
  char mac_text[13];
  snprintf(mac_text, sizeof(mac_text), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  doc["mac"] = mac_text;

#if defined(WITH_WIFI)
  const bool connected = WiFi.status() == WL_CONNECTED;
  doc["wifi"] = connected;
  char ssid[33] = {};
  char ip[16] = {};
  if (connected) {
    strlcpy(ssid, WiFi.SSID().c_str(), sizeof(ssid));
    strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
  }
  doc["ssid"] = ssid;
  doc["ip"] = ip;
#endif

  doc["lat"] = services::location::lat();
  doc["lon"] = services::location::lon();
  doc["tz"] = services::clock::timezone();
  doc["clock"] = services::clock::enabled();
  doc["miles"] = ui::radar::useMiles();
  doc["runways"] = ui::radar::showRunways();
  doc["rings"] = ui::radar::showRings();
  doc["weather"] = services::weather::enabled();
  doc["wxv"] = 1;  // accepts PR:W forecast lines from the USB bridge
  doc["heap"] = ESP.getFreeHeap();
  doc["maxblk"] = ESP.getMaxAllocHeap();
  doc["map"] = static_cast<uint32_t>(services::map::size());
  doc["mapv"] = services::map::kBlobVersion;
  doc["elevv"] = services::terrain::kProtocolVersion;
  doc["otav"] = 2;
  doc["elev"] = services::terrain::available();
  // USB mode (hardware/console.h): "kb" = keyboard mode, uploads need "PR:USBSETUP" first.
  doc["usb"] = console::keyboardMode() ? "kb" : "setup";
#if defined(WITH_DECK)  // without "deckv" the page leaves out the deck step
  doc["deckv"] = services::deck::kVersion;
  doc["deckslots"] = services::deck::kSlots;
  doc["deckicon"] = services::deck::iconPx();
  doc["deckkw"] = services::deck::keyW();  // version 2 sets: pictures fill the key
  doc["deckkh"] = services::deck::keyH();
  {  // version 3 sets: only the used keys, filling this area
    JsonArray area = doc["deckarea"].to<JsonArray>();
    area.add(services::deck::keyAreaW());
    area.add(services::deck::keyAreaH());
    area.add(services::deck::keyGap());
    area.add(services::deck::keyRadius());
  }
  doc["deckcustom"] = services::deck::custom();
  doc["usbfix"] = console::usbRecoveries();       // USB link reconnects since start
  doc["queuev"] = 1;                           // queue panel (PR:QBEGIN ...)
  doc["qthumb"] = services::host::queueThumbPx();  // its small covers, px
  doc["hostv"] = 2;  // Windows helper protocol (services/host_link.h): 2 = windowed cover, "pm"
#endif
#if defined(WITH_WIFI)
  doc["sys"] = 1;
#else
  doc["sys"] = 2;
  doc["feedv"] = 1;  // aircraft, weather and time come from the PC (docs/PROTOCOL.md)
  fillPrefs(doc["prefs"].to<JsonObject>());
#endif
  doc["w"] = config::kDisplayWidth;
  doc["h"] = config::kDisplayHeight;
  sendReply("HELLO", doc);
}

#if defined(WITH_WIFI)
void ensureStaEnabled() {
  const wifi_mode_t mode = WiFi.getMode();
  if ((mode & WIFI_MODE_STA) == 0) {
    WiFi.mode(static_cast<wifi_mode_t>(mode | WIFI_MODE_STA));
  }
}

void sendScan() {
  ensureStaEnabled();
  // Saved network out of reach: the chip keeps trying to join it, and a scan during a
  // join attempt fails at once (WIFI_SCAN_FAILED), so the page saw "no networks". Stop
  // the attempt and the background retry while the page picks a network.
  wifiPauseBackground(120000UL);
  const bool linked = WiFi.status() == WL_CONNECTED;
  if (!linked) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    delay(150);
  }
  int found = WiFi.scanNetworks(false, false);
  for (int retry = 0; found < 0 && retry < 2; ++retry) {
    Console.printf("WiFi scan failed (%d), again\n", found);
    WiFi.scanDelete();
    if (!linked) WiFi.disconnect(false, false);
    delay(400);
    found = WiFi.scanNetworks(false, false);
  }
  if (!linked) WiFi.setAutoReconnect(true);  // the retry starts again after the pause
  if (found < 0) {
    WiFi.scanDelete();
    sendError("scan_failed");
    return;
  }
  JsonDocument doc;
  JsonArray nets = doc["nets"].to<JsonArray>();

  // Strongest first; skip duplicates (same SSID on several access points).
  bool used[64] = {};
  const int limit = found < 64 ? found : 64;
  size_t listed = 0;
  while (listed < kMaxScanResults) {
    int best = -1;
    for (int i = 0; i < limit; ++i) {
      if (!used[i] && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) {
        best = i;
      }
    }
    if (best < 0) {
      break;
    }
    used[best] = true;
    const String name = WiFi.SSID(best);
    if (name.length() == 0) {
      continue;
    }
    bool duplicate = false;
    for (JsonObject n : nets) {
      if (name == n["s"].as<const char*>()) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }
    JsonObject n = nets.add<JsonObject>();
    n["s"] = name;
    n["r"] = WiFi.RSSI(best);
    n["o"] = WiFi.encryptionType(best) == WIFI_AUTH_OPEN;
    ++listed;
  }
  WiFi.scanDelete();
  sendReply("SCAN", doc);
}

struct JoinResult {
  bool ok;
  const char* reason;
};

// Tries the network and, on success, leaves its credentials saved in flash.
// On failure the previous credentials are put back, so a typo cannot lock a
// working device out of its old network.
JoinResult tryJoin(const char* ssid, const char* pass) {
  ensureStaEnabled();

  wifi_config_t previous = {};
  const bool have_previous =
      esp_wifi_get_config(WIFI_IF_STA, &previous) == ESP_OK &&
      previous.sta.ssid[0] != '\0';

  WiFi.persistent(true);
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(ssid, pass);

  const unsigned long started = millis();
  wl_status_t status = WL_IDLE_STATUS;
  while (millis() - started < kJoinTimeoutMs) {
    status = WiFi.status();
    if (status == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      WiFi.setAutoReconnect(true);
      return {true, nullptr};
    }
    if (status == WL_CONNECT_FAILED) {
      break;
    }
    if (status == WL_NO_SSID_AVAIL && millis() - started > kNotFoundGiveUpMs) {
      break;
    }
    delay(100);
  }

  WiFi.disconnect(false, false);
  if (have_previous) {
    esp_wifi_set_config(WIFI_IF_STA, &previous);
  } else {
    WiFi.disconnect(false, true);  // nothing to go back to: forget the attempt
  }

  if (status == WL_NO_SSID_AVAIL) {
    return {false, "ssid_not_found"};
  }
  if (status == WL_CONNECT_FAILED) {
    return {false, "auth_failed"};
  }
  return {false, "timeout"};
}
#endif  // WITH_WIFI

bool optionalBool(const JsonDocument& doc, const char* key, bool fallback) {
  return doc[key].is<bool>() ? doc[key].as<bool>() : fallback;
}

void handleSet(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    sendError("bad_json");
    return;
  }

  // Validate everything first so a bad field never leaves a half-applied set.
  const bool has_lat = doc["lat"].is<double>();
  const bool has_lon = doc["lon"].is<double>();
  if (has_lat != has_lon) {
    sendError("bad_location");
    return;
  }
  double lat = 0.0;
  double lon = 0.0;
  if (has_lat) {
    lat = doc["lat"].as<double>();
    lon = doc["lon"].as<double>();
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
      sendError("bad_location");
      return;
    }
  }

  const char* tz = doc["tz"].is<const char*>() ? doc["tz"].as<const char*>() : nullptr;
  if (tz != nullptr && (tz[0] == '\0' || strlen(tz) > kTzMaxLen)) {
    sendError("bad_timezone");
    return;
  }

#if defined(WITH_WIFI)
  const char* ssid =
      doc["ssid"].is<const char*>() ? doc["ssid"].as<const char*>() : "";
  const char* pass =
      doc["pass"].is<const char*>() ? doc["pass"].as<const char*>() : "";
  if (strlen(ssid) > 32 || strlen(pass) > 63) {
    sendError("bad_wifi");
    return;
  }
  if (ssid[0] != '\0' && pass[0] != '\0' && strlen(pass) < 8) {
    sendError("bad_wifi_password");  // WPA passwords are 8..63 characters
    return;
  }
#endif

  // Settings that do not depend on the network.
  if (has_lat) {
    char lat_text[24];
    char lon_text[24];
    snprintf(lat_text, sizeof(lat_text), "%.6f", lat);
    snprintf(lon_text, sizeof(lon_text), "%.6f", lon);
    services::location::saveFromStrings(lat_text, lon_text);
  }
  if (tz != nullptr || doc["clock"].is<bool>()) {
    char tz_copy[kTzMaxLen + 1];
    strlcpy(tz_copy, tz != nullptr ? tz : services::clock::timezone(),
            sizeof(tz_copy));
    const bool clock_on = optionalBool(doc, "clock", services::clock::enabled());
    services::clock::saveFromPortal(tz_copy, clock_on ? "T" : "");
  }
  if (doc["miles"].is<bool>()) {
    ui::radar::saveMilesFromPortal(doc["miles"].as<bool>() ? "T" : "");
  }
  if (doc["runways"].is<bool>()) {
    ui::radar::saveRunwaysFromPortal(doc["runways"].as<bool>() ? "T" : "");
  }
  if (doc["weather"].is<bool>()) {
    services::weather::saveFromPortal(doc["weather"].as<bool>() ? "T" : "");
  }
  if (doc["rings"].is<bool>()) {
    ui::radar::saveRingsFromPortal(doc["rings"].as<bool>() ? "T" : "");
  }

#if !defined(WITH_WIFI)
  sendOk(false);  // no Wi-Fi: "ssid" / "pass" are ignored
}
#else
  if (ssid[0] == '\0') {
    sendOk(false);
    return;
  }

  // Same network as the saved one and no new password: the page only showed the
  // current name back. Keep the connection (or reuse the saved password) instead
  // of rejoining with an empty password, which would drop a working link.
  char saved_pass[65] = {};
  if (pass[0] == '\0') {
    wifi_config_t saved = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &saved) == ESP_OK &&
        strncmp(reinterpret_cast<const char*>(saved.sta.ssid), ssid, 32) == 0) {
      if (WiFi.status() == WL_CONNECTED) {
        sendOk(false);
        return;
      }
      memcpy(saved_pass, saved.sta.password, 64);
      pass = saved_pass;
    }
  }

  const JoinResult joined = tryJoin(ssid, pass);
  if (!joined.ok) {
    sendError(joined.reason);
    return;
  }

  char ip[16];
  strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
  sendOk(true, ip);
  Console.flush();
  delay(700);
  esp_restart();  // clean start on the saved network
}
#endif  // WITH_WIFI

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decodes standard base64 (with or without '=' padding) into out; -1 on bad input.
int b64Decode(const char* in, uint8_t* out, size_t out_cap) {
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (; *in != '\0' && *in != '='; ++in) {
    const int v = b64Value(*in);
    if (v < 0) {
      return -1;
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= out_cap) {
        return -1;
      }
      out[n++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return static_cast<int>(n);
}

// PR:MAPBEGIN {"len":n,"crc":c}   PR:MAP <seq> <base64>   PR:MAPEND   PR:MAPCLEAR
// Every chunk is acknowledged (PR:MAPACK) so the page never outruns the chip.
void handleMapBegin(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok ||
      !doc["len"].is<uint32_t>() || !doc["crc"].is<uint32_t>()) {
    sendError("bad_json");
    return;
  }
  if (!services::map::beginStaging(doc["len"].as<uint32_t>(),
                                   doc["crc"].as<uint32_t>())) {
    sendError("map_too_big");
    return;
  }
  JsonDocument ack;
  ack["seq"] = 0;
  sendReply("MAPACK", ack);
}

void handleMapChunk(char* args) {
  char* data = strchr(args, ' ');
  if (data == nullptr) {
    sendError("bad_map_chunk");
    return;
  }
  *data++ = '\0';
  const long seq = atol(args);
  uint8_t bytes[480];
  const int n = b64Decode(data, bytes, sizeof(bytes));
  if (n <= 0 || !services::map::stageChunk(bytes, static_cast<size_t>(n))) {
    services::map::abortStaging();
    sendError("bad_map_chunk");
    return;
  }
  JsonDocument ack;
  ack["seq"] = seq;
  sendReply("MAPACK", ack);
}

void handleMapEnd() {
  if (!services::map::commit()) {
    sendError("bad_map");
    return;
  }
  JsonDocument doc;
  doc["map"] = static_cast<uint32_t>(services::map::size());
  sendReply("OK", doc);
}

void handleMapClear() {
  services::map::clear();
  sendOk(false);
}

#if defined(WITH_DECK)
long s_deck_next = 1;  // deck chunk expected next

void handleDeckBegin(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok ||
      !doc["len"].is<uint32_t>() || !doc["crc"].is<uint32_t>()) {
    sendError("bad_json");
    return;
  }
  if (!services::deck::beginStaging(doc["len"].as<uint32_t>(), doc["crc"].as<uint32_t>())) {
    sendError("deck_too_big");
    return;
  }
  s_deck_next = 1;
  JsonDocument ack;
  ack["seq"] = 0;
  sendReply("DECKACK", ack);
}

void handleDeckChunk(char* args) {
  char* data = strchr(args, ' ');
  if (data == nullptr) {
    sendError("bad_deck_chunk");
    return;
  }
  *data++ = '\0';
  const long seq = atol(args);
  JsonDocument ack;
  // Chunks are numbered from 1; the helper sends a few ahead. A repeated one is answered
  // again, a gap asks for the missing one (PR:DECKNAK {"need":n}).
  if (seq > 0 && seq < s_deck_next) {
    ack["seq"] = seq;
    sendReply("DECKACK", ack);
    return;
  }
  if (seq > s_deck_next) {
    ack["need"] = s_deck_next;
    sendReply("DECKNAK", ack);
    return;
  }
  uint8_t bytes[480];
  const int n = b64Decode(data, bytes, sizeof(bytes));
  if (n <= 0 || !services::deck::stageChunk(bytes, static_cast<size_t>(n))) {
    services::deck::abortStaging();
    sendError("bad_deck_chunk");
    return;
  }
  ++s_deck_next;
  ack["seq"] = seq;
  sendReply("DECKACK", ack);
}

void handleDeckEnd() {
  if (!services::deck::commit()) {
    sendError("bad_deck");
    return;
  }
  JsonDocument doc;
  doc["deckcustom"] = services::deck::custom();
  sendReply("OK", doc);
}
#endif

/** Long uploads only in setup mode: the TinyUSB port of keyboard mode stalls on them. */
bool uploadAllowed() {
  if (!console::keyboardMode()) return true;
  sendError("usb_setup");
  return false;
}

void handleReset() {
  sendOk(true);
  Console.flush();
  delay(200);
#if defined(WITH_WIFI)
  wifiResetCredentialsAndReboot();
#else
  // Same settings as the Wi-Fi reset clears (wifi_setup.cpp), without the network.
  services::location::clear();
  services::clock::clear();
  services::weather::clear();
  services::map::clear();
  services::terrain::clear();
  ui::radar::unitsReset();
  esp_restart();
#endif
}

void handleLine(char* line) {
#if defined(WITH_DECK)
  services::perf::line();
#endif
  if (strncmp(line, "PR:", 3) != 0) {
    return;  // ordinary log text
  }
  char* command = line + 3;
  char* args = strchr(command, ' ');
  if (args != nullptr) {
    *args++ = '\0';
  } else {
    args = command + strlen(command);  // empty string
  }

  console::noteActivity();
  if (strcmp(command, "HELLO") == 0) {
    sendHello();
  } else if (strcmp(command, "USBSETUP") == 0) {
    console::switchTo(console::Mode::Setup);
  } else if (strcmp(command, "USBNORMAL") == 0) {
    console::switchTo(console::Mode::Keyboard);
#if defined(WITH_DECK)
  } else if (strcmp(command, "VOL") == 0) {
    services::host::handleVol(args);
  } else if (strcmp(command, "NOW") == 0) {
    services::host::handleNow(args);
  } else if (strcmp(command, "COVERBEGIN") == 0) {
    services::host::handleCoverBegin(args);  // small acked chunks: fine in keyboard mode
  } else if (strcmp(command, "COVER") == 0) {
    services::host::handleCoverChunk(args);
  } else if (strcmp(command, "COVEREND") == 0) {
    services::host::handleCoverEnd();
  } else if (strcmp(command, "QBEGIN") == 0) {  // YouTube Music queue (one acked line each)
    services::host::handleQueueBegin(args);
  } else if (strcmp(command, "QITEM") == 0) {
    services::host::handleQueueItem(args);
  } else if (strcmp(command, "QEND") == 0) {
    services::host::handleQueueEnd();
  } else if (strcmp(command, "QCLEAR") == 0) {
    services::host::handleQueueClear();
  } else if (strcmp(command, "QARTBEGIN") == 0) {
    services::host::handleQueueArtBegin(args);
  } else if (strcmp(command, "QART") == 0) {
    services::host::handleQueueArtChunk(args);
  } else if (strcmp(command, "QARTEND") == 0) {
    services::host::handleQueueArtEnd();
  } else if (strcmp(command, "DECKBEGIN") == 0) {
    if (uploadAllowed()) handleDeckBegin(args);
  } else if (strcmp(command, "DECK") == 0) {
    handleDeckChunk(args);
  } else if (strcmp(command, "DECKEND") == 0) {
    handleDeckEnd();
  } else if (strcmp(command, "DECKCLEAR") == 0) {
    services::deck::clear();
    sendOk(false);
#endif
#if defined(WITH_WIFI)
  } else if (strcmp(command, "SCAN") == 0) {
    sendScan();
#endif
  } else if (strcmp(command, "SET") == 0) {
    handleSet(args);
  } else if (strcmp(command, "MAPBEGIN") == 0) {
    if (uploadAllowed()) handleMapBegin(args);
  } else if (strcmp(command, "MAP") == 0) {
    handleMapChunk(args);
  } else if (strcmp(command, "MAPEND") == 0) {
    handleMapEnd();
  } else if (strcmp(command, "MAPCLEAR") == 0) {
    handleMapClear();
  } else if (strcmp(command, "RESET") == 0) {
    handleReset();
  } else if (strcmp(command, "ELEVROW") == 0) {
    services::terrain::handleRow(args);
  } else if (strcmp(command, "ELEVBEGIN") == 0) {
    if (uploadAllowed()) services::terrain::handleBegin(args);
  } else if (strcmp(command, "ELEVEND") == 0) {
    services::terrain::handleEnd();
  } else if (strcmp(command, "ELEVCLEAR") == 0) {
    services::terrain::handleClear();
  } else if (strcmp(command, "OTA") == 0) {
    services::serial_ota::chunk(args);
  } else if (strcmp(command, "OTABEGIN") == 0) {
    if (uploadAllowed()) services::serial_ota::begin(args);
  } else if (strcmp(command, "OTAEND") == 0) {
    services::serial_ota::end();
  } else if (strcmp(command, "OTAABORT") == 0) {
    services::serial_ota::abort(nullptr);
    sendOk(false);
  } else if (strcmp(command, "W") == 0) {
    services::weather::handleBridge(args);
  } else if (strcmp(command, "A") == 0) {
    services::usb_feed::add(args);
  } else if (strcmp(command, "ABEGIN") == 0) {
    services::usb_feed::begin();
  } else if (strcmp(command, "AEND") == 0) {
    services::usb_feed::end(args);
  } else {
    sendError("unknown_command");
  }
}

}  // namespace

void poll() {
  int budget = services::serial_ota::active() ? 8192 : 2048;  // bytes per call
  while (budget-- > 0 && Console.available() > 0) {
    const int c = Console.read();
    if (c < 0) {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      if (!s_overflow && s_len > 0) {
        s_line[s_len] = '\0';
        handleLine(s_line);
      }
      s_len = 0;
      s_overflow = false;
      continue;
    }
    if (s_len < kMaxLine - 1) {
      s_line[s_len++] = static_cast<char>(c);
    } else {
      s_overflow = true;
    }
  }
#if !defined(WITH_WIFI)
  // A range or unit change on the screen goes to the PC at once (also once at start).
  static int s_sent_range = -1;
  static bool s_sent_miles = false;
  if (!services::serial_ota::active() &&
      (ui::radar::rangeIndex() != s_sent_range || ui::radar::useMiles() != s_sent_miles)) {
    s_sent_range = ui::radar::rangeIndex();
    s_sent_miles = ui::radar::useMiles();
    JsonDocument doc;
    fillPrefs(doc.to<JsonObject>());
    sendReply("PREFS", doc);
  }
#endif
}

}  // namespace services::serial_setup
