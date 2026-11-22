#include "mic_input.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#if MIC_SOURCE == MIC_SOURCE_ICS43434
#include "driver/i2s_std.h"
#else
#include "board_power.h"
#include "driver/i2s_pdm.h"
#endif

#define I2S_PORT I2S_NUM_0
#define MIC_READ_TIMEOUT_MS 1000

#if MIC_SOURCE == MIC_SOURCE_ICS43434
#define MIC_I2S_BCLK GPIO_NUM_26
#define MIC_I2S_WS GPIO_NUM_0
#define MIC_I2S_DATA GPIO_NUM_36
#else
#define MIC_I2S_WS GPIO_NUM_0
#define MIC_I2S_DATA GPIO_NUM_34
#endif

static const char *TAG = "mic_input";
static i2s_chan_handle_t s_rx_channel;

esp_err_t mic_input_init(void)
{
#if MIC_SOURCE == MIC_SOURCE_ICS43434
    // GPIO25 shares the HAT data pin with GPIO36; keep GPIO25 high-impedance.
    const gpio_config_t shared_pin = {
        .pin_bit_mask = 1ULL << GPIO_NUM_25,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&shared_pin), TAG, "configure shared HAT pin");
#else
    ESP_RETURN_ON_ERROR(board_power_enable_microphone(), TAG, "enable onboard microphone");
#endif

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_frame_num = 128;
    channel_config.intr_priority = 1;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_channel), TAG,
                        "create I2S RX channel");

#if MIC_SOURCE == MIC_SOURCE_ICS43434
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK,
            .ws = MIC_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_DATA,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &config), TAG,
                        "configure I2S standard mode");
#else
    i2s_pdm_rx_config_t config = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_I2S_WS,
            .din = MIC_I2S_DATA,
        },
    };
    config.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx_channel, &config), TAG,
                        "configure I2S PDM mode");
#endif
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "enable I2S RX channel");

    ESP_LOGI(TAG, "Microphone source %d ready", MIC_SOURCE);
    return ESP_OK;
}

esp_err_t mic_input_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate != 8000 && sample_rate != 16000) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx_channel), TAG, "disable I2S RX channel");
#if MIC_SOURCE == MIC_SOURCE_ICS43434
    const i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_rx_channel, &clock_config), TAG,
                        "configure I2S sample rate");
#else
    const i2s_pdm_rx_clk_config_t clock_config = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_pdm_rx_clock(s_rx_channel, &clock_config), TAG,
                        "configure PDM sample rate");
#endif
    return i2s_channel_enable(s_rx_channel);
}

esp_err_t mic_input_read(int16_t *samples, size_t sample_count, size_t *samples_read)
{
    if (samples == NULL || samples_read == NULL || sample_count == 0 || sample_count > 120) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_read = 0;
#if MIC_SOURCE == MIC_SOURCE_ICS43434
    int32_t raw[120];
#else
    int16_t raw[120];
#endif

    while (*samples_read < sample_count) {
        const size_t remaining = sample_count - *samples_read;
        size_t bytes_read = 0;
        const esp_err_t err = i2s_channel_read(s_rx_channel, raw, remaining * sizeof(raw[0]),
                                               &bytes_read, MIC_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }

        const size_t raw_samples = bytes_read / sizeof(raw[0]);
        if (raw_samples == 0) {
            continue;
        }

#if MIC_SOURCE == MIC_SOURCE_ICS43434
        for (size_t i = 0; i < raw_samples; ++i) {
            samples[*samples_read + i] = (int16_t)(raw[i] >> 16);
        }
#else
        for (size_t i = 0; i < raw_samples; ++i) {
            samples[*samples_read + i] = raw[i];
        }
#endif
        *samples_read += raw_samples;
    }

    return ESP_OK;
}
