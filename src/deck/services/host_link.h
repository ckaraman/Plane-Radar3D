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
/** True while a cover is coming in (the screen then redraws less often, so each chunk
 *  is answered quickly). */
bool coverReceiving();
uint32_t coverGeneration();

/**
 * The YouTube Music queue from the helper's browser extension, from the current song on:
 *   PR:QBEGIN {"gen":7,"n":12,"src":"YouTube Music"}   then n x
 *   PR:QITEM {"k":0,"x":14,"t":"…","a":"…","d":"3:58"}   (k = row, x = place in the queue)
 *   PR:QEND                                   each answered PR:QACK {}
 *   PR:QCLEAR                                 no queue (another app plays)
 *   PR:QARTBEGIN {"len":n,"crc":c,"k":0,"gen":7} PR:QART <seq> <b64> PR:QARTEND
 *        a row's small cover (JPEG, queueThumbPx() square), chunks as for the album cover
 * To the helper: PR:QPLAY {"x":14,"t":"…"} — play that song.
 */
struct QueueRow {
  char title[64];
  char artist[48];
  char dur[10];
  uint16_t index;            // place in the browser's queue
  const uint8_t* art;        // JPEG or nullptr
  size_t art_len;
};
constexpr int kQueueMax = 30;
bool queueValid();
int queueCount();
const QueueRow& queueRow(int i);
const char* queueSource();
/** Changes with every new queue and every row picture. */
uint32_t queueVersion();
int queueThumbPx();
void sendQueuePlay(int row);

// Line handlers (serial_setup).
void handleQueueBegin(const char* json);
void handleQueueItem(const char* json);
void handleQueueEnd();
void handleQueueClear();
void handleQueueArtBegin(const char* json);
void handleQueueArtChunk(char* args);
void handleQueueArtEnd();
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
