#pragma once

#include "esp_err.h"

esp_err_t board_power_init(void);
esp_err_t board_power_enable_display(void);
esp_err_t board_power_enable_microphone(void);
