#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The Windows helper ("Plane Radar Deck") on the computer, over the USB serial port in
 * keyboard mode. It sends the real volume, what is playing and the album cover; the deck
 * sends it the keys that it carries out itself (open a site in a chosen browser, start a
 * program, ...) and volume changes. Without the helper the deck keeps working as a plain
 * USB keyboard.
 *
 * From the helper (one line each, JSON after the tag):
 *   PR:VOL {"v":42,"m":false}           volume 0..100, muted; also its heartbeat (~2 s)
 *   PR:NOW {"t":"…","a":"…","al":"…","app":"…","d":238,"p":84,"pm":84250,"play":true,"id":7,"back":60}
 *        d / p = duration / position in s (pm: position in ms, newer helpers), id changes
 *        with the track (new cover follows),
 *        back = seconds after a stop until the deck returns to its keys (-1 = never)
 *   PR:NOW {"none":true}                nothing is playing (no media session)
 *   PR:COVERBEGIN {"len":n,"crc":c,"id":7,"w":480,"h":480}  PR:COVER <seq> <base64>  PR:COVEREND
 *        album cover as a baseline JPEG of w x h (at most the panel), shown centred with
 *        its edges fading to black. Chunks are numbered from 1 and answered
 *        PR:COVERACK {"seq":n}; the helper may send a few ahead. A repeated chunk is
 *        acknowledged again, a gap is answered PR:COVERNAK {"need":n} (send again from n).
 * To the helper:
 *   PR:KEY {"i":4}                      helper key i pressed (deck slot)
 *   PR:SETVOL {"v":55}                  knob / bar moved (absolute level)
 *   PR:MUTE {}                          knob tapped
 */
namespace services::host {

/** True while the helper keeps talking (a line in the last 6 s). */
bool active();

int volume();  // 0..100 (last value from the helper)
bool muted();

struct NowPlaying {
  bool valid = false;  // a media session exists
  bool playing = false;
  char title[96] = {};
  char artist[64] = {};
  char album[64] = {};
  char app[32] = {};
  uint32_t duration_s = 0;
  uint32_t position_s = 0;   // whole seconds (as last sent)
  uint32_t pos_ms = 0;       // position in ms at anchor_ms (millis())
  unsigned long anchor_ms = 0;
  uint32_t id = 0;
  int back_s = 60;
};
const NowPlaying& now();
/** Position now (s, ms), extrapolated while playing. */
uint32_t positionNow();
uint32_t positionNowMs();
/** Changes whenever now() changes (title, state, position jump). */
uint32_t nowGeneration();

/** Album cover JPEG (nullptr when none), its size, the track id it belongs to, and its
 *  generation. */
const uint8_t* cover(size_t* len, int* w = nullptr, int* h = nullptr);
uint32_t coverId();
uint32_t coverGeneration();

// Line handlers (serial_setup).
void handleVol(const char* json);
void handleNow(const char* json);
void handleCoverBegin(const char* json);
void handleCoverChunk(char* args);
void handleCoverEnd();

// Events to the helper.
void sendKey(int slot);
void sendVolume(int level);
void sendMute();

}  // namespace services::host
