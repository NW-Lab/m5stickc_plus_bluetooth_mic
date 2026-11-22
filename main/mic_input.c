#include "mic_input.h"

#include "driver/i2s.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if MIC_SOURCE == MIC_SOURCE_ICS43434
#include "driver/gpio.h"
#else
#include "board_power.h"
#endif

#define I2S_PORT I2S_NUM_0

#if MIC_SOURCE == MIC_SOURCE_ICS43434
#define MIC_I2S_BCLK GPIO_NUM_26
#define MIC_I2S_WS GPIO_NUM_0
#define MIC_I2S_DATA GPIO_NUM_36
#define MIC_SAMPLE_BITS I2S_BITS_PER_SAMPLE_32BIT
#define MIC_CHANNEL_FORMAT I2S_CHANNEL_FMT_ONLY_LEFT
#else
#define MIC_I2S_BCLK I2S_PIN_NO_CHANGE
#define MIC_I2S_WS GPIO_NUM_0
#define MIC_I2S_DATA GPIO_NUM_34
#define MIC_SAMPLE_BITS I2S_BITS_PER_SAMPLE_16BIT
#define MIC_CHANNEL_FORMAT I2S_CHANNEL_FMT_ALL_RIGHT
#endif

static const char *TAG = "mic_input";

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

    const i2s_config_t config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX
#if MIC_SOURCE == MIC_SOURCE_STICKC_PLUS
                | I2S_MODE_PDM
#endif
        ,
        .sample_rate = 16000,
        .bits_per_sample = MIC_SAMPLE_BITS,
        .channel_format = MIC_CHANNEL_FORMAT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 6,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };

    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_PORT, &config, 0, NULL), TAG,
                        "install I2S driver");

    const i2s_pin_config_t pins = {
        .bck_io_num = MIC_I2S_BCLK,
        .ws_io_num = MIC_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_I2S_DATA,
        .mck_io_num = I2S_PIN_NO_CHANGE,
    };
    ESP_RETURN_ON_ERROR(i2s_set_pin(I2S_PORT, &pins), TAG, "configure I2S pins");

    ESP_LOGI(TAG, "Microphone source %d ready", MIC_SOURCE);
    return ESP_OK;
}

esp_err_t mic_input_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate != 8000 && sample_rate != 16000) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2s_set_clk(I2S_PORT, sample_rate, MIC_SAMPLE_BITS, I2S_CHANNEL_MONO);
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
        const esp_err_t err = i2s_read(I2S_PORT, raw, remaining * sizeof(raw[0]),
                                       &bytes_read, portMAX_DELAY);
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
