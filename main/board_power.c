#include "board_power.h"

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

#define AXP192_ADDRESS 0x34
#define I2C_TIMEOUT pdMS_TO_TICKS(100)

static const char *TAG = "board_power";

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_write_to_device(I2C_NUM_0, AXP192_ADDRESS, data,
                                      sizeof(data), I2C_TIMEOUT);
}

esp_err_t board_power_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_21,
        .scl_io_num = GPIO_NUM_22,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &config), TAG, "configure I2C");
    return i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

esp_err_t board_power_enable_display(void)
{
    uint8_t reg = 0x12;
    uint8_t enabled;
    ESP_RETURN_ON_ERROR(i2c_master_write_read_device(I2C_NUM_0, AXP192_ADDRESS,
                        &reg, 1, &enabled, 1, I2C_TIMEOUT), TAG, "read LCD power");
    ESP_RETURN_ON_ERROR(write_register(0x28, 0xcc), TAG, "set LCD voltages");
    ESP_RETURN_ON_ERROR(write_register(0x12, enabled | 0x0c), TAG, "enable LCD power");
    return ESP_OK;
}

esp_err_t board_power_enable_microphone(void)
{
    ESP_RETURN_ON_ERROR(write_register(0x91, 0xf0), TAG, "set microphone voltage");
    return write_register(0x90, 0x02);
}
