#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t status_display_init(void);
esp_err_t status_display_update(const char *connection, const char *audio,
                                uint32_t sample_rate, unsigned int level);
