#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Push-to-talk recording via the onboard ES8311 codec + microphone.
 * Produces a WAV buffer (16 kHz, mono, 16-bit) ready for client_send_voice().
 *
 * Proven configuration (docs/hardware.md): ES8311 at I2C 0x30 on the
 * shared bus (SDA GPIO41 / SCL GPIO42, 400 kHz); I2S0 full-duplex with
 * MCLK GPIO13, BCLK GPIO14, WS GPIO47, DIN GPIO21, DOUT GPIO48; NS4150B
 * amp enable on GPIO39. Full-duplex requires in-rate == out-rate: 16 kHz,
 * which is exactly the protocol's WAV baseline — no resampling anywhere.
 * Driver: esp_codec_dev (~1.5.4). Pin defines live in board.h.
 * Max length mirrors Hermes' voice.max_recording_seconds default (120 s)
 * ~= 3.8 MB in PSRAM. */

#define AUDIO_MAX_SECONDS 120

esp_err_t audio_record_start(void);
/* Stops recording; returns a PSRAM buffer the caller frees with free(). */
esp_err_t audio_record_stop(void **wav_out, size_t *len_out);
