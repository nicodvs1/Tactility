// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include <tactility/device.h>
#include <tactility/drivers/gpio.h>

struct St77922Config {
    uint16_t horizontal_resolution;
    uint16_t vertical_resolution;
    bool invert_color;
    bool bgr_order;
    uint32_t pixel_clock_hz;
    uint8_t transaction_queue_depth;
    struct GpioPinSpec pin_reset;
    // Custom vendor init sequence, flattened as bytes: a run of
    // [cmd, data_len, delay_ms, data_len bytes of data...] entries. NULL/0 falls back to the
    // component's built-in default sequence.
    const uint8_t* init_sequence;
    uint32_t init_sequence_length;
    // Optional reference to this display's backlight device, NULL if none.
    struct Device* backlight;
};

#ifdef __cplusplus
}
#endif
