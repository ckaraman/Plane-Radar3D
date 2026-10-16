#pragma once

#include <cstddef>
#include <cstdint>

namespace services::serial_ota {

/**
 * Firmware update over the USB cable while the firmware runs, so the chip never
 * has to be put into its download mode (no BOOT button). The setup page sends:
 *
 *   PR:OTABEGIN {"len":n,"crc":c}   -> PR:OTAACK {"seq":0}
 *   PR:OTA <seq> <len> <base64>      -> PR:OTAACK {"seq":seq}   (seq = 1, 2, ...)
 *                                      or PR:OTANAK {"seq":k}   (resend from chunk k)
 *   PR:OTAEND                        -> PR:OTADONE {} and a restart, or PR:ERR {"msg":...}
 *   PR:OTAABORT                      -> PR:OK {}
 *
 * The page sends one chunk and waits for its answer: while the chip writes flash
 * the UART interrupt is held off and back-to-back lines could lose bytes.
 * HELLO reports "otav":2 for this protocol (v1 = no length field, no NAK).
 * The image goes into the other app slot; the old firmware keeps running until
 * the new one is complete and verified (length, CRC-32 and image check).
 */
void begin(const char* json);
void chunk(char* args);
void end();
void abort(const char* reason);

/** True while an update is being received (the main loop then only polls USB). */
bool active();

/** Called from the main loop: draws progress and cancels a stalled transfer. */
void tick();

}  // namespace services::serial_ota
