/* i2c_bb.h — bit-banged I²C master for the secondary diagnostic bus.
 *
 * ESP32-S3 has only two HW I²C controllers and both are claimed: NUM_0
 * by the main shared bus (MAX17048 + SHT41) and NUM_1 by the camera
 * SCCB driver. The "I²C bus1" diagnostic pads on D6/D7 (GPIO 43/44)
 * therefore can't borrow either HW controller — we bit-bang them in
 * software instead.
 *
 * Open-drain emulation via GPIO_MODE_INPUT_OUTPUT_OD + internal pull-up
 * (~45 kΩ). Suitable for short bench leads to a single SHT41 (≤10 cm)
 * or a quarantined MAX17048 clone. Clocked at ~100 kHz; SHT41's 12 ms
 * measurement window dominates total transaction time anyway.
 *
 * Blocking calls; caller must serialize. No DMA, no IRQs. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotent. Reconfigures both pins as INPUT_OUTPUT_OD with internal
 * pull-up enabled and releases them HIGH. */
esp_err_t i2c_bb_init(int sda_pin, int scl_pin);

/* Force a fresh init even when pins match s_sda/s_scl. Skips the
 * cached early-return so the recovery clock burst + idle re-sample
 * run again. Used by /i2c/bus1_diag to recover a bus that became
 * wedged after boot (e.g. another driver briefly grabbed the pad). */
esp_err_t i2c_bb_reinit(int sda_pin, int scl_pin);

/* True once i2c_bb_init has succeeded with some pin pair. */
bool i2c_bb_initialized(void);

/* Current pin assignment (whichever pair the last successful init
 * picked). -1 when not yet initialised. Non-disturbing — just
 * returns the cached s_sda/s_scl. */
void i2c_bb_pins(int *sda_pin, int *scl_pin);

/* Read live pad levels without disturbing the bus. Returns 0/1 per
 * pin. Sensible only after i2c_bb_init has run; before that, returns
 * -1 in both. */
void i2c_bb_idle_read(int *sda_level, int *scl_level);

/* Drive-low check: actually tries to pull SDA (and separately SCL)
 * low and reads back the pad level. Healthy pad reads 0 when driven
 * low. A damaged pad / IO buffer that lost its output stage will
 * stay at 1 (pulled-up by the internal/external pullup, with no
 * effective drain). Releases both lines back to HIGH afterward.
 * Returns -1 in either field if bus not initialised. */
void i2c_bb_drive_low_check(int *sda_low_readback, int *scl_low_readback);

/* SDA↔SCL short detection. Drives one line low, samples the other.
 * `*sda_to_scl_bleed` = 1 means pulling SDA down also pulled SCL down
 * (and vice-versa for `scl_to_sda_bleed`). Either bleed=1 → the bus
 * is electrically tied and START can never form, so no slave will
 * ever ACK regardless of how healthy each individual pad looks. */
void i2c_bb_short_check(int *sda_to_scl_bleed, int *scl_to_sda_bleed);

/* Address probe: START + write address with W bit, observe ACK, STOP.
 * Returns true on ACK. Safe to call repeatedly. */
bool i2c_bb_probe(uint8_t addr);

/* Single START → addr(W) → bytes → STOP transaction. Returns
 * ESP_ERR_NOT_FOUND if the address itself NACKs (no device), ESP_FAIL
 * on data NACK, ESP_OK on full transmission. */
esp_err_t i2c_bb_transmit(uint8_t addr, const uint8_t *buf, size_t len);

/* Single START → addr(R) → read `len` bytes (master ACKs all but the
 * last) → STOP. ESP_ERR_NOT_FOUND on address NACK. */
esp_err_t i2c_bb_receive(uint8_t addr, uint8_t *buf, size_t len);

/* Register read: START → addr(W) → wbuf bytes → repeated START → addr(R)
 * → read rbuf bytes → STOP. The standard SMBus-style transaction every
 * 16-bit register chip uses (MAX17048 0x02 VCELL, etc). ESP_ERR_NOT_FOUND
 * on either address NACK; ESP_FAIL on data NACK during the write phase. */
esp_err_t i2c_bb_transmit_receive(uint8_t addr, const uint8_t *wbuf, size_t wlen,
                                  uint8_t *rbuf, size_t rlen);

#ifdef __cplusplus
}
#endif
