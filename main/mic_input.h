#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MIC_SOURCE_ICS43434 1
#define MIC_SOURCE_STICKC_PLUS 2

#ifndef MIC_SOURCE
#define MIC_SOURCE MIC_SOURCE_ICS43434
#endif

#if MIC_SOURCE != MIC_SOURCE_ICS43434 && MIC_SOURCE != MIC_SOURCE_STICKC_PLUS
#error "MIC_SOURCE must be MIC_SOURCE_ICS43434 or MIC_SOURCE_STICKC_PLUS"
#endif

esp_err_t mic_input_init(void);
esp_err_t mic_input_set_sample_rate(uint32_t sample_rate);
esp_err_t mic_input_read(int16_t *samples, size_t sample_count, size_t *samples_read);
