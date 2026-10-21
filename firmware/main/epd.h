#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* SSD1677 e-paper driver (800x480 panel on SPI3).
 *
 * Provenance: written from the SSD1677 datasheet command table (Solomon
 * Systech) and the panel parameters in docs/hardware.md. It is an
 * independent implementation — no code from the GPL-licensed
 * waveshare-airecorder driver was used; only hardware facts (pins, busy
 * polarity, reset timings, refresh-mode repertoire) were cross-checked
 * against that project's hardware specification.
 *
 * The framebuffer lives in PSRAM (48,000 bytes, 1 = white, 0 = black),
 * plus a previous-frame copy used as the "old" plane for partial updates.
 * Waveform/update-sequence constants flagged BENCH are the datasheet
 * defaults for OTP operation; confirm on first hardware bring-up. */

esp_err_t epd_init(void);

/* Framebuffer access for the renderer. */
uint8_t *epd_framebuffer(void);
void epd_clear(bool white);
void epd_draw_pixel(int x, int y, bool black);

/* Refresh modes (see docs/hardware.md):
 *  full    — OTP full update; clears ghosting. ~seconds.
 *  fast    — OTP fast waveform where the panel OTP provides one.
 *  partial — old-plane differential update; quick, accumulates ghosting.
 * All refresh entry points wake the panel from sleep automatically. */
esp_err_t epd_refresh_full(void);
esp_err_t epd_refresh_fast(void);
esp_err_t epd_refresh_partial(void);

/* Panel deep sleep (command 0x10). Only a hardware reset wakes it;
 * the refresh functions handle that transparently. */
esp_err_t epd_sleep(void);
bool epd_is_sleeping(void);
