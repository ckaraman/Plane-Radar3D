#pragma once

#include <esp_arduino_version.h>

#include <WiFiClient.h>

namespace services::tls {

/**
 * HTTPS transport on BearSSL with fixed memory.
 *
 * mbedTLS (WiFiClientSecure) needs ~50-60 KB of heap per session -- two 16 KB
 * record buffers plus handshake state, allocated anew for every connection. Next
 * to the 76.8 KB frame buffer the ESP32 heap cannot provide that (mbedTLS -32512).
 * BearSSL runs in one 16.7 KB buffer (half-duplex) and a ~4 KB context and
 * allocates nothing during the handshake. That memory is static, so it cannot
 * fragment.
 *
 * There is ONE engine: only one Client may be connected at a time (connecting
 * one closes the other). Certificates are not verified (same as the
 * WiFiClientSecure::setInsecure() this replaces); the leaf certificate's key is
 * still used, so the session is encrypted.
 *
 * Derives from WiFiClient so HTTPClient::begin(client, url) takes it.
 */
class Client : public WiFiClient {
 public:
  Client() = default;
  ~Client();

  int connect(IPAddress ip, uint16_t port) override;
  int connect(IPAddress ip, uint16_t port, int32_t timeout_ms) override;
  int connect(const char* host, uint16_t port) override;
  int connect(const char* host, uint16_t port, int32_t timeout_ms) override;
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override {}
  void stop() override;
  uint8_t connected() override;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // Core 3.x: Stream::setTimeout(ms) is not virtual; HTTPClient sets it in ms.
  void setTimeout(unsigned long ms) { Stream::setTimeout(ms); io_ms_ = ms < 1000 ? 1000 : ms; }
#else
  int setTimeout(uint32_t seconds) override;
#endif

  // WiFiClientSecure-compatible knobs.
  void setInsecure() {}
  void setHandshakeTimeout(unsigned long seconds) { handshake_ms_ = seconds * 1000UL; }
  /** Last BearSSL error code (0 = none); text in buf. */
  int lastError(char* buf, size_t size);

 private:
  int runUntil(unsigned target, bool wait, unsigned long timeout_ms);
  bool owns() const;

  WiFiClient tcp_;
  bool active_ = false;
  unsigned long handshake_ms_ = 8000;
  unsigned long io_ms_ = 6000;
  int last_error_ = 0;
};

}  // namespace services::tls
