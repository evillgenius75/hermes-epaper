#include "audio.h"

#include "board.h"
#include "esp_log.h"

static const char *TAG = "audio";

/* TODO(hw): implement with esp_codec_dev against the proven configuration
 * in audio.h / board.h: create the I2C + I2S data interfaces, open the
 * ES8311 (addr BOARD_ES8311_ADDR, PA pin BOARD_PA_GPIO), record 16 kHz
 * mono 16-bit PCM into PSRAM, and wrap it in a WAV header on stop. */

esp_err_t audio_record_start(void)
{
    ESP_LOGW(TAG, "audio_record_start: not implemented (TODO(hw))");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_record_stop(void **wav_out, size_t *len_out)
{
    (void)wav_out;
    (void)len_out;
    ESP_LOGW(TAG, "audio_record_stop: not implemented (TODO(hw))");
    return ESP_ERR_NOT_SUPPORTED;
}
