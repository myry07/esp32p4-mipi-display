#include <inttypes.h>
#include "audio_player.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

static const char *TAG = "audio_player";
extern i2s_chan_handle_t i2s_tx_handle;

static esp_err_t my_mute(AUDIO_PLAYER_MUTE_SETTING setting) {
    ESP_LOGI(TAG, "mute = %d", setting);
    return ESP_OK;
}

static esp_err_t my_clk_set(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t mode) {
    ESP_LOGI(TAG, "set clk: %" PRIu32 " Hz, %" PRIu32 " bits, mode %d", rate, bits_cfg, mode);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    return i2s_channel_reconfig_std_clock(i2s_tx_handle, &clk_cfg);
}

static esp_err_t my_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms) {
    return i2s_channel_write(i2s_tx_handle, audio_buffer, len, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

void mp3_play_start(void) {
    audio_player_config_t cfg = {
        .mute_fn   = my_mute,
        .clk_set_fn = my_clk_set,
        .write_fn  = my_write,
        .priority  = 5,
        .coreID    = 0,
    };

    if (audio_player_new(cfg) != ESP_OK) {
        ESP_LOGE(TAG, "audio_player_new failed");
        return;
    }

    FILE *fp = fopen("/sdcard/test.mp3", "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen failed");
        return;
    }

    audio_player_play(fp);
    fclose(fp);

    audio_player_delete();
}