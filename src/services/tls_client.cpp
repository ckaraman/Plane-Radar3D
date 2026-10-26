#include "services/tls_client.h"
#include "hardware/console.h"

#include <Arduino.h>
#include <esp_system.h>

#include <cstring>

#include <bearssl.h>

namespace services::tls {

namespace {

// ------------------------------------------------------------------ insecure X.509
// Accepts any chain (like setInsecure()), but decodes the leaf certificate so the
// handshake can use the server's public key.
struct InsecureX509 {
  const br_x509_class* vtable;
  br_x509_decoder_context dec;
  int depth;
};

void xStartChain(const br_x509_class** ctx, const char*) {
  reinterpret_cast<InsecureX509*>(ctx)->depth = 0;
}
void xStartCert(const br_x509_class** ctx, uint32_t) {
  auto* c = reinterpret_cast<InsecureX509*>(ctx);
  if (c->depth == 0) br_x509_decoder_init(&c->dec, nullptr, nullptr);
}
void xAppend(const br_x509_class** ctx, const unsigned char* buf, size_t len) {
  auto* c = reinterpret_cast<InsecureX509*>(ctx);
  if (c->depth == 0) br_x509_decoder_push(&c->dec, buf, len);
}
void xEndCert(const br_x509_class** ctx) { ++reinterpret_cast<InsecureX509*>(ctx)->depth; }
unsigned xEndChain(const br_x509_class** ctx) {
  auto* c = reinterpret_cast<InsecureX509*>(ctx);
  if (c->depth == 0) return BR_ERR_X509_EMPTY_CHAIN;
  const int err = br_x509_decoder_last_error(&c->dec);
  if (err != 0) return static_cast<unsigned>(err);
  return br_x509_decoder_get_pkey(&c->dec) != nullptr ? 0 : BR_ERR_X509_EMPTY_CHAIN;
}
const br_x509_pkey* xGetPkey(const br_x509_class* const* ctx, unsigned* usages) {
  auto* c = reinterpret_cast<InsecureX509*>(const_cast<const br_x509_class**>(ctx));
  if (usages != nullptr) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
  return br_x509_decoder_get_pkey(&c->dec);
}
const br_x509_class kInsecureVtable = {sizeof(InsecureX509), xStartChain, xStartCert, xAppend,
                                       xEndCert,             xEndChain,   xGetPkey};

// ------------------------------------------------------------------ the one engine
// Static on purpose: taken once at link time, never fragmented.
br_ssl_client_context s_sc;
InsecureX509 s_x509;
unsigned char s_iobuf[BR_SSL_BUFSIZE_MONO];
Client* s_owner = nullptr;

void initEngine() {
  // TLS 1.2 with forward-secret AEAD suites first; CBC kept as a fallback.
  static const uint16_t kSuites[] = {
      BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
      BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
      BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
      BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
      BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
      BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
      BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
      BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
      BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
      BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
      BR_TLS_RSA_WITH_AES_128_CBC_SHA,
  };
  br_ssl_client_zero(&s_sc);
  br_ssl_engine_set_versions(&s_sc.eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_suites(&s_sc.eng, kSuites, sizeof(kSuites) / sizeof(kSuites[0]));
  br_ssl_client_set_default_rsapub(&s_sc);
  br_ssl_engine_set_default_rsavrfy(&s_sc.eng);
  br_ssl_engine_set_default_ecdsa(&s_sc.eng);
  br_ssl_engine_set_hash(&s_sc.eng, br_sha1_ID, &br_sha1_vtable);
  br_ssl_engine_set_hash(&s_sc.eng, br_sha224_ID, &br_sha224_vtable);
  br_ssl_engine_set_hash(&s_sc.eng, br_sha256_ID, &br_sha256_vtable);
  br_ssl_engine_set_hash(&s_sc.eng, br_sha384_ID, &br_sha384_vtable);
  br_ssl_engine_set_hash(&s_sc.eng, br_sha512_ID, &br_sha512_vtable);
  br_ssl_engine_set_prf_sha256(&s_sc.eng, &br_tls12_sha256_prf);
  br_ssl_engine_set_prf_sha384(&s_sc.eng, &br_tls12_sha384_prf);
  br_ssl_engine_set_default_aes_gcm(&s_sc.eng);
  br_ssl_engine_set_default_aes_cbc(&s_sc.eng);
  br_ssl_engine_set_default_chapol(&s_sc.eng);
  s_x509.vtable = &kInsecureVtable;
  br_ssl_engine_set_x509(&s_sc.eng, &s_x509.vtable);
  br_ssl_engine_set_buffer(&s_sc.eng, s_iobuf, sizeof(s_iobuf), 0);  // half-duplex
  unsigned char seed[32];
  esp_fill_random(seed, sizeof(seed));
  br_ssl_engine_inject_entropy(&s_sc.eng, seed, sizeof(seed));
}

}  // namespace

Client::~Client() { stop(); }

bool Client::owns() const { return active_ && s_owner == this; }

int Client::connect(IPAddress, uint16_t) { return 0; }
int Client::connect(IPAddress, uint16_t, int32_t) { return 0; }
int Client::connect(const char* host, uint16_t port) {
  return connect(host, port, static_cast<int32_t>(handshake_ms_));
}

int Client::connect(const char* host, uint16_t port, int32_t timeout_ms) {
  stop();
  if (s_owner != nullptr && s_owner != this) s_owner->stop();  // one engine
  last_error_ = 0;
  if (!tcp_.connect(host, port, timeout_ms)) {
    last_error_ = -1;
    return 0;
  }
  tcp_.setNoDelay(true);
  initEngine();
  if (!br_ssl_client_reset(&s_sc, host, 0)) {
    last_error_ = br_ssl_engine_last_error(&s_sc.eng);
    tcp_.stop();
    return 0;
  }
  s_owner = this;
  active_ = true;
  if (runUntil(BR_SSL_SENDAPP | BR_SSL_RECVAPP, true, handshake_ms_) <= 0) {
    if (last_error_ == 0) last_error_ = br_ssl_engine_last_error(&s_sc.eng);
    if (last_error_ == 0) last_error_ = -2;  // timeout
    Console.printf("tls: handshake with %s failed (%d)\n", host, last_error_);
    stop();
    return 0;
  }
  return 1;
}

/**
 * Drive the engine until a `target` state bit is set (1), it would need bytes
 * that have not arrived and `wait` is false (0), or it failed / closed (-1).
 * Records to send are always written out first.
 */
int Client::runUntil(unsigned target, bool wait, unsigned long timeout_ms) {
  if (!owns()) return -1;
  br_ssl_engine_context* eng = &s_sc.eng;
  const unsigned long deadline = millis() + timeout_ms;
  unsigned flushed_state = 0;  // state at the last flush that changed nothing
  for (;;) {
    const unsigned st = br_ssl_engine_current_state(eng);
    if (st & BR_SSL_CLOSED) {
      last_error_ = br_ssl_engine_last_error(eng);
      return -1;
    }
    if (st & BR_SSL_SENDREC) {
      size_t len = 0;
      unsigned char* buf = br_ssl_engine_sendrec_buf(eng, &len);
      const size_t w = tcp_.write(buf, len);
      if (w == 0) {
        if (!tcp_.connected() || static_cast<long>(millis() - deadline) > 0) {
          last_error_ = BR_ERR_IO;
          tcp_.stop();
          return -1;
        }
        delay(1);
        continue;
      }
      br_ssl_engine_sendrec_ack(eng, w);
      continue;
    }
    if (st & target) return 1;
    if (st & BR_SSL_RECVAPP) return -1;  // unread data while asked to send (half-duplex)
    if (st & BR_SSL_RECVREC) {
      const int avail = tcp_.available();
      if (avail <= 0) {
        if (!tcp_.connected()) {
          last_error_ = BR_ERR_IO;
          tcp_.stop();
          return -1;
        }
        if (!wait) return 0;
        if (static_cast<long>(millis() - deadline) > 0) return -1;
        delay(1);
        continue;
      }
      size_t len = 0;
      unsigned char* buf = br_ssl_engine_recvrec_buf(eng, &len);
      const int r = tcp_.read(buf, len < static_cast<size_t>(avail) ? len : static_cast<size_t>(avail));
      if (r <= 0) continue;
      br_ssl_engine_recvrec_ack(eng, static_cast<size_t>(r));
      continue;
    }
    if (flushed_state == st) {  // flushing again would change nothing
      if (!wait || static_cast<long>(millis() - deadline) > 0) return wait ? -1 : 0;
      delay(1);
    }
    flushed_state = st;
    br_ssl_engine_flush(eng, 0);
  }
}

size_t Client::write(uint8_t b) { return write(&b, 1); }

size_t Client::write(const uint8_t* buf, size_t size) {
  if (!owns()) return 0;
  size_t done = 0;
  while (done < size) {
    if (runUntil(BR_SSL_SENDAPP, true, io_ms_) <= 0) return done;
    size_t alen = 0;
    unsigned char* app = br_ssl_engine_sendapp_buf(&s_sc.eng, &alen);
    const size_t n = (size - done) < alen ? (size - done) : alen;
    memcpy(app, buf + done, n);
    br_ssl_engine_sendapp_ack(&s_sc.eng, n);
    done += n;
  }
  br_ssl_engine_flush(&s_sc.eng, 0);
  // Push the records out now (HTTP sends the request, then waits for the answer).
  if (runUntil(BR_SSL_SENDAPP | BR_SSL_RECVAPP, true, io_ms_) < 0) return 0;
  return done;
}

int Client::available() {
  if (!owns()) return 0;
  size_t len = 0;
  if (br_ssl_engine_recvapp_buf(&s_sc.eng, &len) != nullptr && len > 0) return static_cast<int>(len);
  if (runUntil(BR_SSL_RECVAPP, false, 0) <= 0) return 0;
  br_ssl_engine_recvapp_buf(&s_sc.eng, &len);
  return static_cast<int>(len);
}

int Client::read(uint8_t* buf, size_t size) {
  if (available() <= 0) return -1;
  size_t len = 0;
  unsigned char* app = br_ssl_engine_recvapp_buf(&s_sc.eng, &len);
  const size_t n = size < len ? size : len;
  memcpy(buf, app, n);
  br_ssl_engine_recvapp_ack(&s_sc.eng, n);
  return static_cast<int>(n);
}

int Client::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

int Client::peek() {
  if (available() <= 0) return -1;
  size_t len = 0;
  const unsigned char* app = br_ssl_engine_recvapp_buf(&s_sc.eng, &len);
  return app[0];
}

uint8_t Client::connected() {
  if (!owns()) return 0;
  size_t len = 0;
  if (br_ssl_engine_recvapp_buf(&s_sc.eng, &len) != nullptr && len > 0) return 1;
  if (br_ssl_engine_current_state(&s_sc.eng) & BR_SSL_CLOSED) return 0;
  return tcp_.connected();
}

void Client::stop() {
  if (owns()) s_owner = nullptr;
  active_ = false;
  tcp_.stop();
}

#if !(defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3)
int Client::setTimeout(uint32_t seconds) {
  io_ms_ = seconds * 1000UL;
  if (io_ms_ < 1000) io_ms_ = 1000;
  return 0;
}
#endif

int Client::lastError(char* buf, size_t size) {
  if (buf != nullptr && size > 0) snprintf(buf, size, "BearSSL %d", last_error_);
  return last_error_;
}

}  // namespace services::tls
