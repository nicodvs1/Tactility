// SPDX-License-Identifier: Apache-2.0
#include <drivers/st77922.h>
#include <st77922_module.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/esp32_spi.h>
#include <tactility/drivers/spi_controller.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77922.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdlib>
#include <cstring>

constexpr auto* TAG = "ST77922";

#define GET_CONFIG(device) (static_cast<const St77922Config*>((device)->config))

// The ST77922 only accepts draw windows whose X start and width are multiples of 4 (other values
// garble the image). There is no such requirement for Y.
constexpr int32_t X_ALIGNMENT = 4;

// MIRROR/SWAP_XY are deliberately not advertised: the panel has no hardware axis swap (MADCTL.MV is
// not validated for this panel), so the LVGL bridge falls back to software rotation.
#define ST77922_CAPABILITIES (DISPLAY_CAPABILITY_INVERT_COLOR | DISPLAY_CAPABILITY_ON_OFF | \
    DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM)

struct St77922Internal {
    Device* spi_controller;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    // draw_bitmap() must block until the transfer physically completes (DisplayApi's synchronous
    // contract); see gc9a01-module for the same pattern.
    SemaphoreHandle_t draw_done_semaphore;
    // Heap-allocated only when the devicetree supplies a custom init_sequence. Its .data pointers
    // alias into the devicetree's static const byte buffer, so only the array itself is freed.
    st77922_lcd_init_cmd_t* parsed_init_cmds;
    // Full-screen copy of what the panel is currently showing, in the panel's byte order. When
    // LVGL hands us a region that isn't 4-aligned on X, the extra border pixels come from here.
    uint8_t* shadow;
    // Gather buffer for the (possibly widened) aligned window handed to the panel.
    uint8_t* staging;
};

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user_ctx) {
    auto* internal = static_cast<St77922Internal*>(user_ctx);
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(internal->draw_done_semaphore, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static int pin_or_unused(const struct GpioPinSpec& pin) {
    return pin.gpio_controller == nullptr ? -1 : static_cast<int>(pin.pin);
}

// Unpacks the devicetree's flat [cmd, data_len, delay_ms, data_len bytes...] encoding into a
// heap-allocated st77922_lcd_init_cmd_t array (same encoding as st7123-module).
static bool parse_init_sequence(const uint8_t* bytes, uint32_t length, st77922_lcd_init_cmd_t** out_cmds, uint16_t* out_count) {
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < length; count++) {
        if (offset + 3 > length) {
            LOG_E(TAG, "init-sequence truncated: entry header runs past the end of the array");
            return false;
        }
        offset += 3 + bytes[offset + 1];
        if (offset > length) {
            LOG_E(TAG, "init-sequence truncated: entry data runs past the end of the array");
            return false;
        }
    }

    auto* cmds = static_cast<st77922_lcd_init_cmd_t*>(malloc(count * sizeof(st77922_lcd_init_cmd_t)));
    if (cmds == nullptr) {
        return false;
    }

    uint32_t offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t data_len = bytes[offset + 1];
        cmds[i] = {
            .cmd = bytes[offset],
            .data = data_len > 0 ? &bytes[offset + 3] : nullptr,
            .data_bytes = data_len,
            .delay_ms = bytes[offset + 2],
        };
        offset += 3 + data_len;
    }

    *out_cmds = cmds;
    *out_count = static_cast<uint16_t>(count);
    return true;
}

// Sends an already aligned, tightly-packed region to the panel and blocks until the last chunk has
// physically completed.
static error_t present(St77922Internal* internal, int32_t x_start, int32_t y_start, int32_t x_end, int32_t y_end, const void* data) {
    xSemaphoreTake(internal->draw_done_semaphore, 0);

    spi_controller_lock(internal->spi_controller);
    esp_err_t ret = esp_lcd_panel_draw_bitmap(internal->panel_handle, x_start, y_start, x_end, y_end, data);
    if (ret != ESP_OK) {
        spi_controller_unlock(internal->spi_controller);
        return ERROR_RESOURCE;
    }

    // Hold the bus lock across the wait too: the command/data phases aren't wrapped in their own
    // acquire_bus by esp_lcd_panel_io_spi, so another bus user could otherwise interleave with a
    // transfer that's still in flight.
    xSemaphoreTake(internal->draw_done_semaphore, portMAX_DELAY);
    spi_controller_unlock(internal->spi_controller);
    return ERROR_NONE;
}

static void free_internal(St77922Internal* internal) {
    free(internal->shadow);
    free(internal->staging);
    free(internal->parsed_init_cmds);
    if (internal->draw_done_semaphore != nullptr) {
        vSemaphoreDelete(internal->draw_done_semaphore);
    }
    free(internal);
}

// region Driver lifecycle

static error_t start(Device* device) {
    auto* parent = device_get_parent(device);
    check(device_get_type(parent) == &SPI_CONTROLLER_TYPE);

    const auto* spi_config = static_cast<const Esp32SpiConfig*>(parent->config);
    const auto* config = GET_CONFIG(device);

    if (config->horizontal_resolution % X_ALIGNMENT != 0) {
        LOG_E(TAG, "horizontal-resolution must be a multiple of %d", (int)X_ALIGNMENT);
        return ERROR_INVALID_ARGUMENT;
    }

    struct GpioPinSpec cs_pin;
    if (esp32_spi_get_cs_pin(device, &cs_pin) != ERROR_NONE) {
        LOG_E(TAG, "Failed to resolve CS pin");
        return ERROR_RESOURCE;
    }

    auto* internal = static_cast<St77922Internal*>(calloc(1, sizeof(St77922Internal)));
    if (internal == nullptr) {
        return ERROR_OUT_OF_MEMORY;
    }

    internal->spi_controller = parent;
    internal->draw_done_semaphore = xSemaphoreCreateBinary();
    if (internal->draw_done_semaphore == nullptr) {
        free_internal(internal);
        return ERROR_OUT_OF_MEMORY;
    }

    const size_t frame_buffer_size = static_cast<size_t>(config->horizontal_resolution) * config->vertical_resolution * 2;
    internal->shadow = static_cast<uint8_t*>(malloc(frame_buffer_size));
    internal->staging = static_cast<uint8_t*>(malloc(frame_buffer_size));
    if (internal->shadow == nullptr || internal->staging == nullptr) {
        LOG_E(TAG, "Failed to allocate frame buffers");
        free_internal(internal);
        return ERROR_OUT_OF_MEMORY;
    }
    std::memset(internal->shadow, 0, frame_buffer_size);

    uint16_t init_cmds_size = 0;
    if (config->init_sequence != nullptr && config->init_sequence_length > 0) {
        if (!parse_init_sequence(config->init_sequence, config->init_sequence_length, &internal->parsed_init_cmds, &init_cmds_size)) {
            LOG_E(TAG, "Failed to parse init-sequence property");
            free_internal(internal);
            return ERROR_INVALID_ARGUMENT;
        }
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = static_cast<gpio_num_t>(pin_or_unused(cs_pin)),
        .dc_gpio_num = static_cast<gpio_num_t>(-1),
        .spi_mode = 0,
        .pclk_hz = config->pixel_clock_hz,
        .trans_queue_depth = config->transaction_queue_depth,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = internal,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            // The LVGL color buffer lives in PSRAM (see PREFER_EXTERNAL_RAM); ESP32-S3 can DMA from
            // PSRAM directly, which avoids bouncing every chunk through internal RAM (see sh8601-module).
            .psram_dma_direct = 1,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };

    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_config->host, &io_config, &internal->io_handle);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        free_internal(internal);
        return ERROR_RESOURCE;
    }

    st77922_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = internal->parsed_init_cmds;
    vendor_config.init_cmds_size = init_cmds_size;
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {
        .rgb_ele_order = config->bgr_order ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
        .reset_gpio_num = static_cast<gpio_num_t>(pin_or_unused(config->pin_reset)),
        .vendor_config = &vendor_config,
        .flags = { .reset_active_high = false },
    };

    ret = esp_lcd_new_panel_st77922(internal->io_handle, &panel_config, &internal->panel_handle);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to create panel: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(internal->io_handle);
        free_internal(internal);
        return ERROR_RESOURCE;
    }

    spi_controller_lock(internal->spi_controller);
    bool ok =
        esp_lcd_panel_reset(internal->panel_handle) == ESP_OK &&
        esp_lcd_panel_init(internal->panel_handle) == ESP_OK;
    ok = ok && esp_lcd_panel_invert_color(internal->panel_handle, config->invert_color) == ESP_OK;
    ok = ok && esp_lcd_panel_disp_on_off(internal->panel_handle, true) == ESP_OK;
    spi_controller_unlock(internal->spi_controller);

    if (!ok) {
        LOG_E(TAG, "Failed to bring up panel");
        esp_lcd_panel_del(internal->panel_handle);
        esp_lcd_panel_io_del(internal->io_handle);
        free_internal(internal);
        return ERROR_RESOURCE;
    }

    device_set_driver_data(device, internal);

    // Wipe the panel's undefined power-up GRAM content, in full-width (thus aligned) bands so the
    // panel IO never queues several large chunks at once.
    constexpr int32_t CLEAR_BAND_HEIGHT = 32;
    for (int32_t y = 0; y < config->vertical_resolution; y += CLEAR_BAND_HEIGHT) {
        int32_t y_end = y + CLEAR_BAND_HEIGHT;
        if (y_end > config->vertical_resolution) {
            y_end = config->vertical_resolution;
        }
        if (present(internal, 0, y, config->horizontal_resolution, y_end, internal->shadow + static_cast<size_t>(y) * config->horizontal_resolution * 2) != ERROR_NONE) {
            LOG_W(TAG, "Failed to clear panel");
            break;
        }
    }

    return ERROR_NONE;
}

static error_t stop(Device* device) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));

    spi_controller_lock(internal->spi_controller);
    if (internal->panel_handle != nullptr) {
        if (esp_lcd_panel_del(internal->panel_handle) != ESP_OK) {
            LOG_E(TAG, "Failed to delete panel");
            spi_controller_unlock(internal->spi_controller);
            return ERROR_RESOURCE;
        }
        internal->panel_handle = nullptr;
    }

    if (internal->io_handle != nullptr) {
        if (esp_lcd_panel_io_del(internal->io_handle) != ESP_OK) {
            LOG_E(TAG, "Failed to delete panel IO");
            spi_controller_unlock(internal->spi_controller);
            return ERROR_RESOURCE;
        }
        internal->io_handle = nullptr;
    }
    spi_controller_unlock(internal->spi_controller);

    free_internal(internal);
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

// endregion

// region DisplayApi

static error_t st77922_reset(Device* device) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);
    spi_controller_lock(internal->spi_controller);
    error_t result = esp_lcd_panel_reset(internal->panel_handle) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
    spi_controller_unlock(internal->spi_controller);
    if (result == ERROR_NONE) {
        // The reset drops the panel's image, so the shadow no longer describes it.
        std::memset(internal->shadow, 0, static_cast<size_t>(config->horizontal_resolution) * config->vertical_resolution * 2);
    }
    return result;
}

static error_t st77922_init(Device* device) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));
    spi_controller_lock(internal->spi_controller);
    error_t result = esp_lcd_panel_init(internal->panel_handle) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
    spi_controller_unlock(internal->spi_controller);
    return result;
}

static error_t st77922_draw_bitmap(Device* device, int32_t x_start, int32_t y_start, int32_t x_end, int32_t y_end, const void* color_data) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);
    const int32_t hres = config->horizontal_resolution;
    const int32_t vres = config->vertical_resolution;

    if (x_start < 0) x_start = 0;
    if (y_start < 0) y_start = 0;
    if (x_end > hres) x_end = hres;
    if (y_end > vres) y_end = vres;
    if (x_start >= x_end || y_start >= y_end) {
        return ERROR_NONE;
    }

    const size_t shadow_stride = static_cast<size_t>(hres) * 2;

    // Keep the shadow in sync with the pixels LVGL just handed us. Row-by-row because the incoming
    // region is tightly packed while the shadow spans the full screen.
    const int32_t width = x_end - x_start;
    const int32_t height = y_end - y_start;
    const auto* source = static_cast<const uint8_t*>(color_data);
    for (int32_t row = 0; row < height; row++) {
        std::memcpy(
            internal->shadow + static_cast<size_t>(y_start + row) * shadow_stride + static_cast<size_t>(x_start) * 2,
            source + static_cast<size_t>(row) * width * 2,
            static_cast<size_t>(width) * 2
        );
    }

    // Widen to the 4-aligned X window the ST77922 requires. hres is a multiple of 4 (checked in
    // start()), so the aligned end never exceeds it.
    const int32_t aligned_x_start = x_start & ~(X_ALIGNMENT - 1);
    const int32_t aligned_x_end = (x_end + X_ALIGNMENT - 1) & ~(X_ALIGNMENT - 1);

    if (aligned_x_start == x_start && aligned_x_end == x_end) {
        return present(internal, x_start, y_start, x_end, y_end, source);
    }

    const int32_t aligned_width = aligned_x_end - aligned_x_start;
    uint8_t* destination = internal->staging;
    for (int32_t row = y_start; row < y_end; row++) {
        std::memcpy(
            destination,
            internal->shadow + static_cast<size_t>(row) * shadow_stride + static_cast<size_t>(aligned_x_start) * 2,
            static_cast<size_t>(aligned_width) * 2
        );
        destination += static_cast<size_t>(aligned_width) * 2;
    }

    return present(internal, aligned_x_start, y_start, aligned_x_end, y_end, internal->staging);
}

static error_t st77922_mirror(Device*, bool, bool) {
    return ERROR_NOT_SUPPORTED;
}

static error_t st77922_swap_xy(Device*, bool) {
    return ERROR_NOT_SUPPORTED;
}

static error_t st77922_invert_color(Device* device, bool invert_color_data) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));
    spi_controller_lock(internal->spi_controller);
    error_t result = esp_lcd_panel_invert_color(internal->panel_handle, invert_color_data) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
    spi_controller_unlock(internal->spi_controller);
    return result;
}

static error_t st77922_disp_on_off(Device* device, bool on_off) {
    auto* internal = static_cast<St77922Internal*>(device_get_driver_data(device));
    spi_controller_lock(internal->spi_controller);
    error_t result = esp_lcd_panel_disp_on_off(internal->panel_handle, on_off) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
    spi_controller_unlock(internal->spi_controller);
    return result;
}

// The panel is big-endian while the OS is little-endian, so every pixel's two bytes are swapped.
static enum DisplayColorFormat st77922_get_color_format(Device*) {
    return DISPLAY_COLOR_FORMAT_RGB565_SWAPPED;
}

static uint16_t st77922_get_resolution_x(Device* device) {
    return GET_CONFIG(device)->horizontal_resolution;
}

static uint16_t st77922_get_resolution_y(Device* device) {
    return GET_CONFIG(device)->vertical_resolution;
}

static error_t st77922_get_backlight(Device* device, Device** backlight) {
    auto* configured_backlight = GET_CONFIG(device)->backlight;
    if (configured_backlight == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    *backlight = configured_backlight;
    return ERROR_NONE;
}

static bool st77922_has_capability(Device* device, uint32_t capability) {
    uint32_t capabilities = ST77922_CAPABILITIES;
    if (GET_CONFIG(device)->backlight != nullptr) {
        capabilities |= DISPLAY_CAPABILITY_BACKLIGHT;
    }
    return (capabilities & capability) == capability;
}

static const DisplayApi st77922_display_api = {
    .capabilities = ST77922_CAPABILITIES,
    .reset = st77922_reset,
    .init = st77922_init,
    .draw_bitmap = st77922_draw_bitmap,
    .clear = nullptr,
    .refresh = nullptr,
    .mirror = st77922_mirror,
    .swap_xy = st77922_swap_xy,
    .get_swap_xy = nullptr,
    .get_mirror_x = nullptr,
    .get_mirror_y = nullptr,
    .set_gap = nullptr,
    .get_gap_x = nullptr,
    .get_gap_y = nullptr,
    .invert_color = st77922_invert_color,
    .disp_on_off = st77922_disp_on_off,
    .disp_sleep = nullptr,
    .get_color_format = st77922_get_color_format,
    .get_resolution_x = st77922_get_resolution_x,
    .get_resolution_y = st77922_get_resolution_y,
    // No direct frame buffer access: the panel streams from an LVGL-owned buffer.
    .get_frame_buffer = nullptr,
    .get_frame_buffer_count = nullptr,
    .get_backlight = st77922_get_backlight,
    .has_capability = st77922_has_capability,
    .get_shape = nullptr,
};

Driver st77922_driver = {
    .name = "st77922",
    .compatible = (const char*[]) { "sitronix,st77922", nullptr },
    .start_device = start,
    .stop_device = stop,
    .probe = nullptr,
    .api = &st77922_display_api,
    .device_type = &DISPLAY_TYPE,
    .internal = nullptr
};

// endregion
