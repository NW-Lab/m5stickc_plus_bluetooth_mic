#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board_power.h"
#include "mic_input.h"
#include "status_display.h"

#define DEVICE_NAME "M5 Bluetooth Mic"
#define TX_STREAM_SIZE 2048
#define HFP_RETRY_INTERVAL_MS 5000

#define PROFILE_READY_BIT BIT0
#define PEER_KNOWN_BIT BIT1
#define PEER_DIRTY_BIT BIT2
#define SLC_CONNECTED_BIT BIT3
#define AUDIO_CONNECTING_BIT BIT4
#define AUDIO_CONNECTED_BIT BIT5
#define HFP_CONNECTING_BIT BIT6
#define MIC_ERROR_BIT BIT7
#define HFP_ERROR_BIT BIT8

static const char *TAG = "bluetooth_mic";
static EventGroupHandle_t s_events;
static StreamBufferHandle_t s_audio_stream;
static SemaphoreHandle_t s_audio_stream_mutex;
static esp_bd_addr_t s_peer_address;
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_rate_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_audio_sample_rate;
static uint32_t s_mic_peak;
static int64_t s_last_tx_us;

static bool copy_peer_address(esp_bd_addr_t address)
{
    if ((xEventGroupGetBits(s_events) & PEER_KNOWN_BIT) == 0) {
        return false;
    }

    portENTER_CRITICAL(&s_peer_mux);
    memcpy(address, s_peer_address, sizeof(s_peer_address));
    portEXIT_CRITICAL(&s_peer_mux);
    return true;
}

static void remember_peer_address(const esp_bd_addr_t address)
{
    portENTER_CRITICAL(&s_peer_mux);
    memcpy(s_peer_address, address, sizeof(s_peer_address));
    portEXIT_CRITICAL(&s_peer_mux);
    xEventGroupSetBits(s_events, PEER_KNOWN_BIT | PEER_DIRTY_BIT);
}

static uint32_t get_audio_sample_rate(void)
{
    portENTER_CRITICAL(&s_rate_mux);
    const uint32_t sample_rate = s_audio_sample_rate;
    portEXIT_CRITICAL(&s_rate_mux);
    return sample_rate;
}

static void set_audio_sample_rate(uint32_t sample_rate)
{
    portENTER_CRITICAL(&s_rate_mux);
    s_audio_sample_rate = sample_rate;
    portEXIT_CRITICAL(&s_rate_mux);
}

static void save_peer_address(void)
{
    esp_bd_addr_t address;
    if (!copy_peer_address(address)) {
        return;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open("bluetooth_mic", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS for peer address: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, "peer", address, sizeof(address));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save paired device: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved paired Windows device");
    }
}

static void load_peer_address(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("bluetooth_mic", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    ESP_ERROR_CHECK(err);

    esp_bd_addr_t address;
    size_t size = sizeof(address);
    err = nvs_get_blob(handle, "peer", address, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    ESP_ERROR_CHECK(err);
    if (size != sizeof(address)) {
        ESP_LOGE(TAG, "Stored Bluetooth address has an invalid size");
        return;
    }

    remember_peer_address(address);
    xEventGroupClearBits(s_events, PEER_DIRTY_BIT);
}

static void hfp_incoming_data_callback(const uint8_t *data, uint32_t length)
{
    (void)data;
    (void)length;
}

static uint32_t hfp_outgoing_data_callback(uint8_t *data, uint32_t length)
{
    if (s_audio_stream == NULL ||
        (xEventGroupGetBits(s_events) & AUDIO_CONNECTED_BIT) == 0 ||
        xSemaphoreTake(s_audio_stream_mutex, 0) != pdTRUE) {
        return 0;
    }

    size_t received = 0;
    if ((xEventGroupGetBits(s_events) & AUDIO_CONNECTED_BIT) != 0 &&
        xStreamBufferBytesAvailable(s_audio_stream) >= length) {
        received = xStreamBufferReceive(s_audio_stream, data, length, 0);
    }
    xSemaphoreGive(s_audio_stream_mutex);
    if (received == length && received > 0) {
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_stats_mux);
        s_last_tx_us = now;
        portEXIT_CRITICAL(&s_stats_mux);
    }
    return received == length ? (uint32_t)received : 0;
}

static void hfp_client_callback(esp_hf_client_cb_event_t event,
                                esp_hf_client_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (param->prof_stat.state == ESP_HF_INIT_SUCCESS) {
            ESP_ERROR_CHECK(esp_hf_client_register_data_callback(
                hfp_incoming_data_callback, hfp_outgoing_data_callback));
            xEventGroupSetBits(s_events, PROFILE_READY_BIT);
            ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(
                ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
            ESP_LOGI(TAG, "HFP ready; pair this device with Windows");
        } else {
            xEventGroupSetBits(s_events, HFP_ERROR_BIT);
            ESP_LOGE(TAG, "HFP profile initialization failed: %d",
                     param->prof_stat.state);
        }
        break;

    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            remember_peer_address(param->conn_stat.remote_bda);
            xEventGroupClearBits(s_events, HFP_CONNECTING_BIT);
            xEventGroupSetBits(s_events, SLC_CONNECTED_BIT);
            ESP_LOGI(TAG, "HFP service-level connection established");
            const esp_err_t err = esp_hf_client_connect_audio(param->conn_stat.remote_bda);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Could not request HFP audio: %s", esp_err_to_name(err));
            } else {
                xEventGroupSetBits(s_events, AUDIO_CONNECTING_BIT);
            }
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING ||
                   param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED) {
            xEventGroupSetBits(s_events, HFP_CONNECTING_BIT);
        } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            xEventGroupClearBits(s_events, SLC_CONNECTED_BIT | AUDIO_CONNECTING_BIT |
                                             AUDIO_CONNECTED_BIT | HFP_CONNECTING_BIT);
            set_audio_sample_rate(0);
            ESP_LOGI(TAG, "HFP disconnected");
        }
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            portENTER_CRITICAL(&s_stats_mux);
            s_last_tx_us = 0;
            s_mic_peak = 0;
            portEXIT_CRITICAL(&s_stats_mux);
            set_audio_sample_rate(8000);
            xEventGroupClearBits(s_events, AUDIO_CONNECTING_BIT);
            xEventGroupSetBits(s_events, AUDIO_CONNECTED_BIT);
            ESP_LOGI(TAG, "HFP audio connected using CVSD (8 kHz)");
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            portENTER_CRITICAL(&s_stats_mux);
            s_last_tx_us = 0;
            s_mic_peak = 0;
            portEXIT_CRITICAL(&s_stats_mux);
            set_audio_sample_rate(16000);
            xEventGroupClearBits(s_events, AUDIO_CONNECTING_BIT);
            xEventGroupSetBits(s_events, AUDIO_CONNECTED_BIT);
            ESP_LOGI(TAG, "HFP audio connected using mSBC (16 kHz)");
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTING) {
            xEventGroupSetBits(s_events, AUDIO_CONNECTING_BIT);
        } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            set_audio_sample_rate(0);
            xEventGroupClearBits(s_events, AUDIO_CONNECTING_BIT | AUDIO_CONNECTED_BIT);
            portENTER_CRITICAL(&s_stats_mux);
            s_last_tx_us = 0;
            s_mic_peak = 0;
            portEXIT_CRITICAL(&s_stats_mux);
            ESP_LOGI(TAG, "HFP audio disconnected");
        }
        break;

    default:
        break;
    }
}

static void bluetooth_gap_callback(esp_bt_gap_cb_event_t event,
                                   esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            remember_peer_address(param->auth_cmpl.bda);
            ESP_LOGI(TAG, "Bluetooth pairing complete with %s",
                     param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Bluetooth pairing failed: status %d",
                     param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "Confirming Bluetooth pairing");
        ESP_ERROR_CHECK(esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true));
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        const uint8_t pin_length = param->pin_req.min_16_digit ? 16 : 4;
        if (param->pin_req.min_16_digit) {
            memset(pin_code, '0', sizeof(pin_code));
        }
        ESP_ERROR_CHECK(esp_bt_gap_pin_reply(param->pin_req.bda, true, pin_length,
                                             pin_code));
        break;
    }

    default:
        break;
    }
}

static void microphone_task(void *arg)
{
    (void)arg;
    int16_t samples[120];
    uint32_t active_rate = 0;
    int64_t last_overrun_log_us = 0;
    uint32_t dropped_frames = 0;

    while (true) {
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & AUDIO_CONNECTED_BIT) == 0) {
            if (active_rate != 0) {
                xSemaphoreTake(s_audio_stream_mutex, portMAX_DELAY);
                xStreamBufferReset(s_audio_stream);
                xSemaphoreGive(s_audio_stream_mutex);
                active_rate = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const uint32_t sample_rate = get_audio_sample_rate();
        if (sample_rate == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (sample_rate != active_rate) {
            ESP_ERROR_CHECK(mic_input_set_sample_rate(sample_rate));
            xSemaphoreTake(s_audio_stream_mutex, portMAX_DELAY);
            xStreamBufferReset(s_audio_stream);
            xSemaphoreGive(s_audio_stream_mutex);
            active_rate = sample_rate;
            ESP_LOGI(TAG, "Microphone sample rate set to %lu Hz",
                     (unsigned long)sample_rate);
        }

        const size_t frame_samples = sample_rate == 8000 ? 30 : 120;
        size_t samples_read = 0;
        const esp_err_t err = mic_input_read(samples, frame_samples, &samples_read);
        if (err != ESP_OK) {
            xEventGroupSetBits(s_events, MIC_ERROR_BIT);
            ESP_LOGE(TAG, "Microphone read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (samples_read != frame_samples) {
            xEventGroupSetBits(s_events, MIC_ERROR_BIT);
            ESP_LOGE(TAG, "Microphone returned an incomplete audio frame");
            continue;
        }
        xEventGroupClearBits(s_events, MIC_ERROR_BIT);
        uint32_t peak = 0;
        for (size_t i = 0; i < samples_read; ++i) {
            const int32_t sample = samples[i];
            const uint32_t magnitude = sample < 0 ? (uint32_t)-sample : (uint32_t)sample;
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        portENTER_CRITICAL(&s_stats_mux);
        if (peak > s_mic_peak) {
            s_mic_peak = peak;
        }
        portEXIT_CRITICAL(&s_stats_mux);

        const size_t frame_bytes = frame_samples * sizeof(samples[0]);
        if (xSemaphoreTake(s_audio_stream_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            ++dropped_frames;
            continue;
        }
        if (xStreamBufferSpacesAvailable(s_audio_stream) < frame_bytes) {
            xSemaphoreGive(s_audio_stream_mutex);
            ++dropped_frames;
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_overrun_log_us >= 2000000) {
                ESP_LOGW(TAG, "Dropped %lu microphone frames due to audio backlog",
                         (unsigned long)dropped_frames);
                dropped_frames = 0;
                last_overrun_log_us = now_us;
            }
            continue;
        }

        const size_t sent = xStreamBufferSend(s_audio_stream, samples, frame_bytes, 0);
        xSemaphoreGive(s_audio_stream_mutex);
        if (sent != frame_bytes) {
            ESP_LOGE(TAG, "Could not queue a complete microphone frame");
            continue;
        }
        esp_hf_client_outgoing_data_ready();
    }
}

static void display_task(void *arg)
{
    (void)arg;
    while (true) {
        const EventBits_t state = xEventGroupGetBits(s_events);
        const char *connection = (state & HFP_ERROR_BIT) ? "BT ERROR" :
                                 (state & SLC_CONNECTED_BIT) ? "CONNECTED" :
                                 (state & HFP_CONNECTING_BIT) ? "CONNECTING" :
                                 !(state & PROFILE_READY_BIT) ? "STARTING" :
                                 (state & PEER_KNOWN_BIT) ? "WAIT PC" : "PAIR MODE";

        portENTER_CRITICAL(&s_stats_mux);
        const int64_t last_tx = s_last_tx_us;
        const uint32_t peak = s_mic_peak;
        s_mic_peak = 0;
        portEXIT_CRITICAL(&s_stats_mux);

        const bool audio_connected = (state & AUDIO_CONNECTED_BIT) != 0;
        const bool tx_active = last_tx != 0 && esp_timer_get_time() - last_tx < 1000000;
        const char *audio = (state & MIC_ERROR_BIT) && audio_connected ? "MIC ERROR" :
                            !audio_connected ? "TX OFF" :
                            tx_active ? "TX LIVE" : "TX WAIT";
        const uint32_t rate = audio_connected ? get_audio_sample_rate() : 0;
        const unsigned int level = audio_connected ? (unsigned int)(peak * 100U / 32768U) : 0;
        const esp_err_t err = status_display_update(connection, audio, rate, level);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LCD update failed: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void bluetooth_manager_task(void *arg)
{
    (void)arg;
    TickType_t last_connection_attempt = 0;
    TickType_t last_audio_attempt = 0;

    while (true) {
        const EventBits_t bits = xEventGroupWaitBits(
            s_events, PEER_DIRTY_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(250));
        if ((bits & PEER_DIRTY_BIT) != 0) {
            save_peer_address();
        }

        const TickType_t now = xTaskGetTickCount();
        const EventBits_t state = xEventGroupGetBits(s_events);
        if ((state & PROFILE_READY_BIT) != 0 &&
            (state & (SLC_CONNECTED_BIT | HFP_CONNECTING_BIT)) == 0 &&
            (state & PEER_KNOWN_BIT) != 0 &&
            now - last_connection_attempt >= pdMS_TO_TICKS(HFP_RETRY_INTERVAL_MS)) {
            esp_bd_addr_t address;
            if (copy_peer_address(address)) {
                xEventGroupSetBits(s_events, HFP_CONNECTING_BIT);
                const esp_err_t err = esp_hf_client_connect(address);
                if (err != ESP_OK) {
                    xEventGroupClearBits(s_events, HFP_CONNECTING_BIT);
                    ESP_LOGW(TAG, "HFP reconnect request failed: %s",
                             esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "Connecting to paired Windows device");
                }
            }
            last_connection_attempt = now;
        }

        if ((state & SLC_CONNECTED_BIT) != 0 &&
            (state & (AUDIO_CONNECTED_BIT | AUDIO_CONNECTING_BIT)) == 0 &&
            now - last_audio_attempt >= pdMS_TO_TICKS(HFP_RETRY_INTERVAL_MS)) {
            esp_bd_addr_t address;
            if (copy_peer_address(address)) {
                const esp_err_t err = esp_hf_client_connect_audio(address);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "HFP audio reconnect request failed: %s",
                             esp_err_to_name(err));
                } else {
                    xEventGroupSetBits(s_events, AUDIO_CONNECTING_BIT);
                }
            }
            last_audio_attempt = now;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "Could not create Bluetooth state event group");
        abort();
    }
    s_audio_stream = xStreamBufferCreate(TX_STREAM_SIZE, 1);
    if (s_audio_stream == NULL) {
        ESP_LOGE(TAG, "Could not create HFP audio stream buffer");
        abort();
    }
    s_audio_stream_mutex = xSemaphoreCreateMutex();
    if (s_audio_stream_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create HFP audio stream mutex");
        abort();
    }

    ESP_ERROR_CHECK(board_power_init());
    ESP_ERROR_CHECK(status_display_init());
    ESP_ERROR_CHECK(status_display_update("STARTING", "TX OFF", 0, 0));
    ESP_ERROR_CHECK(mic_input_init());
    load_peer_address();

    BaseType_t task_result = xTaskCreate(display_task, "display", 3072, NULL, 2, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start display task");
        abort();
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_config));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(DEVICE_NAME));
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bluetooth_gap_callback));
    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE,
                                                  &io_capability, sizeof(uint8_t)));

    ESP_ERROR_CHECK(esp_hf_client_register_callback(hfp_client_callback));
    ESP_ERROR_CHECK(esp_hf_client_init());

    task_result = xTaskCreate(microphone_task, "microphone",
                              4096, NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start microphone task");
        abort();
    }
    task_result = xTaskCreate(bluetooth_manager_task, "bluetooth_manager",
                              4096, NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start Bluetooth manager task");
        abort();
    }
}
