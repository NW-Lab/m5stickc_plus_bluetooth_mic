#include "status_display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_power.h"
#include "mic_input.h"

#define LCD_WIDTH 135
#define LCD_HEIGHT 240
#define ROW_HEIGHT 24
#define TEXT_SCALE 2
#define RGB565(r, g, b) ((((r) & 0x1f) << 11) | (((g) & 0x3f) << 5) | ((b) & 0x1f))
#define COLOR_BG RGB565(0, 0, 2)
#define COLOR_TEXT RGB565(31, 63, 31)
#define COLOR_ACCENT RGB565(0, 50, 25)
#define COLOR_DIM RGB565(5, 15, 8)

static const char *TAG = "status_display";
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;
static DMA_ATTR uint16_t s_pixels[LCD_WIDTH * ROW_HEIGHT];
static char s_previous_connection[12];
static char s_previous_audio[12];
static uint32_t s_previous_rate = UINT32_MAX;
static unsigned int s_previous_level = UINT32_MAX;

/* Five-bit rows, top to bottom. */
static const uint8_t s_letters[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};

static const uint8_t s_digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
};

static bool transfer_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)io;
    (void)event;
    BaseType_t awoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &awoken);
    return awoken == pdTRUE;
}

static uint16_t wire_color(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static void fill_row(uint16_t color)
{
    const uint16_t pixel = wire_color(color);
    for (size_t i = 0; i < LCD_WIDTH * ROW_HEIGHT; ++i) {
        s_pixels[i] = pixel;
    }
}

static void draw_text(const char *text, uint16_t color)
{
    const uint16_t pixel = wire_color(color);
    for (int index = 0; text[index] && index < 11; ++index) {
        const char ch = text[index];
        const uint8_t *glyph = NULL;
        if (ch >= 'A' && ch <= 'Z') {
            glyph = s_letters[ch - 'A'];
        } else if (ch >= '0' && ch <= '9') {
            glyph = s_digits[ch - '0'];
        }
        if (glyph == NULL) {
            continue;
        }
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] & (1U << (4 - col))) == 0) {
                    continue;
                }
                for (int dy = 0; dy < TEXT_SCALE; ++dy) {
                    for (int dx = 0; dx < TEXT_SCALE; ++dx) {
                        const int x = 4 + index * 6 * TEXT_SCALE + col * TEXT_SCALE + dx;
                        const int y = 5 + row * TEXT_SCALE + dy;
                        s_pixels[y * LCD_WIDTH + x] = pixel;
                    }
                }
            }
        }
    }
}

static esp_err_t send_row(int y)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH,
                                                  y + ROW_HEIGHT, s_pixels),
                        TAG, "draw LCD row");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "LCD transfer timed out");
    return ESP_OK;
}

static esp_err_t text_row(int y, const char *text, uint16_t color)
{
    fill_row(COLOR_BG);
    draw_text(text, color);
    return send_row(y);
}

esp_err_t status_display_init(void)
{
    ESP_RETURN_ON_ERROR(board_power_enable_display(), TAG, "enable LCD power");
    s_transfer_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_transfer_done != NULL, ESP_ERR_NO_MEM, TAG, "LCD semaphore");

    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_13,
        .mosi_io_num = GPIO_NUM_15,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_pixels),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize LCD SPI");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = GPIO_NUM_23,
        .cs_gpio_num = GPIO_NUM_5,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = transfer_done,
        .user_ctx = s_transfer_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                &io_config, &io), TAG, "LCD SPI IO");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_18,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel),
                        TAG, "create ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "initialize LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_NORON, NULL, 0),
                        TAG, "set LCD normal mode");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 52, 40), TAG, "set LCD offset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert LCD colors");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "turn on LCD");

    fill_row(COLOR_BG);
    for (int y = 0; y < LCD_HEIGHT; y += ROW_HEIGHT) {
        ESP_RETURN_ON_ERROR(send_row(y), TAG, "clear LCD");
    }
    ESP_RETURN_ON_ERROR(text_row(0, "BT MIC", COLOR_ACCENT), TAG, "draw title");
#if MIC_SOURCE == MIC_SOURCE_ICS43434
    ESP_RETURN_ON_ERROR(text_row(30, "ICS 43434", COLOR_TEXT), TAG, "draw source");
#else
    ESP_RETURN_ON_ERROR(text_row(30, "ONBOARD MIC", COLOR_TEXT), TAG, "draw source");
#endif
    ESP_RETURN_ON_ERROR(text_row(170, "MIC LEVEL", COLOR_TEXT), TAG, "draw level label");
    return ESP_OK;
}

esp_err_t status_display_update(const char *connection, const char *audio,
                                uint32_t sample_rate, unsigned int level)
{
    if (strcmp(connection, s_previous_connection) != 0) {
        ESP_RETURN_ON_ERROR(text_row(65, connection, COLOR_TEXT), TAG, "draw connection");
        snprintf(s_previous_connection, sizeof(s_previous_connection), "%s", connection);
    }
    if (strcmp(audio, s_previous_audio) != 0) {
        ESP_RETURN_ON_ERROR(text_row(100, audio, COLOR_ACCENT), TAG, "draw audio");
        snprintf(s_previous_audio, sizeof(s_previous_audio), "%s", audio);
    }
    if (sample_rate != s_previous_rate) {
        const char *rate = sample_rate == 8000 ? "RATE 8KHZ" :
                           sample_rate == 16000 ? "RATE 16KHZ" : "RATE OFF";
        ESP_RETURN_ON_ERROR(text_row(135, rate, COLOR_TEXT), TAG, "draw rate");
        s_previous_rate = sample_rate;
    }
    if (level > 100) {
        level = 100;
    }
    if (level != s_previous_level) {
        fill_row(COLOR_BG);
        const int length = (int)(level * 125 / 100);
        for (int y = 7; y < 17; ++y) {
            for (int x = 0; x < 125; ++x) {
                s_pixels[y * LCD_WIDTH + 5 + x] = wire_color(x < length ? COLOR_ACCENT : COLOR_DIM);
            }
        }
        ESP_RETURN_ON_ERROR(send_row(200), TAG, "draw microphone level");
        s_previous_level = level;
    }
    return ESP_OK;
}
