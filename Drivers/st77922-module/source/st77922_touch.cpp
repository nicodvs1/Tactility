// SPDX-License-Identifier: Apache-2.0
#include <drivers/st77922_touch.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/gpio.h>
#include <tactility/drivers/gpio_controller.h>
#include <tactility/drivers/i2c_controller.h>
#include <tactility/drivers/pointer.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdlib>

constexpr auto* TAG = "ST77922Touch";
#define GET_CONFIG(device) (static_cast<const St77922TouchConfig*>((device)->config))

// Register map and read logic ported from the xiaozhi-esp32 lcdwiki-es3c35p board, proven on real
// hardware. Registers have 16-bit (big-endian) addresses.
static constexpr uint16_t REG_MAX_TOUCHES = 0x0009;
static constexpr uint16_t REG_INFO = 0x0010;
static constexpr uint16_t REG_POINT0 = 0x0014;
static constexpr uint8_t INFO_DATA_READY = 0x08;
static constexpr uint8_t POINT_VALID = 0x80;
static constexpr uint8_t POINT_DATA_LEN = 7;
static constexpr uint8_t MAX_POINTS = 5;
static constexpr TickType_t I2C_TIMEOUT = pdMS_TO_TICKS(100);

struct St77922TouchInternal {
    GpioDescriptor* reset_descriptor;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    uint8_t max_points;
    uint8_t point_count;
    uint16_t x[MAX_POINTS];
    uint16_t y[MAX_POINTS];
};

static error_t read_register(Device* device, uint16_t reg, uint8_t* data, size_t length) {
    const auto* config = GET_CONFIG(device);
    const uint8_t reg_bytes[2] = { static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF) };
    return i2c_controller_write_read(device_get_parent(device), config->address, reg_bytes, sizeof(reg_bytes), data, length, I2C_TIMEOUT);
}

// region Driver lifecycle

static error_t reset_pulse(GpioDescriptor* descriptor) {
    bool ok = gpio_descriptor_set_level(descriptor, true) == ERROR_NONE;
    vTaskDelay(pdMS_TO_TICKS(10));
    ok = ok && gpio_descriptor_set_level(descriptor, false) == ERROR_NONE;
    // The controller needs time to boot before it answers on I2C.
    vTaskDelay(pdMS_TO_TICKS(100));

    return ok ? ERROR_NONE : ERROR_RESOURCE;
}

static error_t start(Device* device) {
    auto* parent = device_get_parent(device);
    check(device_get_type(parent) == &I2C_CONTROLLER_TYPE);

    const auto* config = GET_CONFIG(device);

    auto* internal = static_cast<St77922TouchInternal*>(malloc(sizeof(St77922TouchInternal)));
    if (internal == nullptr) {
        return ERROR_OUT_OF_MEMORY;
    }
    internal->reset_descriptor = nullptr;
    internal->swap_xy = config->swap_xy;
    internal->mirror_x = config->mirror_x;
    internal->mirror_y = config->mirror_y;
    internal->max_points = 1;
    internal->point_count = 0;

    if (config->pin_reset.gpio_controller != nullptr) {
        internal->reset_descriptor = gpio_descriptor_acquire(config->pin_reset.gpio_controller, config->pin_reset.pin, GPIO_FLAG_DIRECTION_OUTPUT | GPIO_FLAG_ACTIVE_LOW, GPIO_OWNER_GPIO);
        if (internal->reset_descriptor == nullptr) {
            LOG_E(TAG, "Failed to acquire reset pin");
            free(internal);
            return ERROR_RESOURCE;
        }

        if (reset_pulse(internal->reset_descriptor) != ERROR_NONE) {
            LOG_E(TAG, "Failed to pulse reset pin");
            gpio_descriptor_release(internal->reset_descriptor);
            free(internal);
            return ERROR_RESOURCE;
        }
    }

    // A failed read keeps the single-point default: the controller still reports point 0.
    uint8_t max_points = 0;
    if (read_register(device, REG_MAX_TOUCHES, &max_points, 1) == ERROR_NONE && max_points > 0 && max_points <= MAX_POINTS) {
        internal->max_points = max_points;
    }

    device_set_driver_data(device, internal);
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));

    if (internal->reset_descriptor != nullptr) {
        gpio_descriptor_release(internal->reset_descriptor);
    }

    free(internal);
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

// endregion

// region PointerApi

static error_t st77922_touch_read_data(Device* device, TickType_t) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));

    uint8_t info = 0;
    error_t error = read_register(device, REG_INFO, &info, 1);
    if (error != ERROR_NONE) {
        return error;
    }

    internal->point_count = 0;
    if ((info & INFO_DATA_READY) == 0) {
        return ERROR_NONE;
    }

    // All points must be read in one transaction, or the controller doesn't clear its buffer.
    uint8_t data[POINT_DATA_LEN * MAX_POINTS];
    error = read_register(device, REG_POINT0, data, POINT_DATA_LEN * internal->max_points);
    if (error != ERROR_NONE) {
        return error;
    }

    for (uint8_t i = 0; i < internal->max_points; i++) {
        const uint8_t* point = &data[i * POINT_DATA_LEN];
        if ((point[0] & POINT_VALID) == 0) {
            continue;
        }
        internal->x[internal->point_count] = static_cast<uint16_t>(((point[0] & 0x3F) << 8) | point[1]);
        internal->y[internal->point_count] = static_cast<uint16_t>(((point[2] & 0x3F) << 8) | point[3]);
        internal->point_count++;
    }

    return ERROR_NONE;
}

static bool st77922_touch_get_touched_points(Device* device, uint16_t* x, uint16_t* y, uint16_t* strength, uint8_t* point_count, uint8_t max_point_count) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);

    uint8_t count = internal->point_count;
    if (count > max_point_count) {
        count = max_point_count;
    }

    // Order matches esp_lcd_touch's own software coordinate adjustment (mirror X, mirror Y,
    // then swap) so behavior lines up with the other touch modules in this codebase.
    for (uint8_t i = 0; i < count; i++) {
        uint16_t point_x = internal->x[i];
        uint16_t point_y = internal->y[i];
        if (internal->mirror_x) {
            point_x = config->x_max - point_x;
        }
        if (internal->mirror_y) {
            point_y = config->y_max - point_y;
        }
        if (internal->swap_xy) {
            uint16_t tmp = point_x;
            point_x = point_y;
            point_y = tmp;
        }
        x[i] = point_x;
        y[i] = point_y;
        if (strength != nullptr) {
            strength[i] = 0;
        }
    }

    *point_count = count;
    return count > 0;
}

static error_t st77922_touch_set_swap_xy(Device* device, bool swap) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    internal->swap_xy = swap;
    return ERROR_NONE;
}

static error_t st77922_touch_get_swap_xy(Device* device, bool* swap) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    *swap = internal->swap_xy;
    return ERROR_NONE;
}

static error_t st77922_touch_set_mirror_x(Device* device, bool mirror) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    internal->mirror_x = mirror;
    return ERROR_NONE;
}

static error_t st77922_touch_get_mirror_x(Device* device, bool* mirror) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    *mirror = internal->mirror_x;
    return ERROR_NONE;
}

static error_t st77922_touch_set_mirror_y(Device* device, bool mirror) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    internal->mirror_y = mirror;
    return ERROR_NONE;
}

static error_t st77922_touch_get_mirror_y(Device* device, bool* mirror) {
    auto* internal = static_cast<St77922TouchInternal*>(device_get_driver_data(device));
    *mirror = internal->mirror_y;
    return ERROR_NONE;
}

// endregion

static const PointerApi st77922_touch_pointer_api = {
    // Unsupported (null per PointerApi's contract): the reference implementation has no sleep mode.
    .enter_sleep = nullptr,
    .exit_sleep = nullptr,
    .read_data = st77922_touch_read_data,
    .get_touched_points = st77922_touch_get_touched_points,
    .set_swap_xy = st77922_touch_set_swap_xy,
    .get_swap_xy = st77922_touch_get_swap_xy,
    .set_mirror_x = st77922_touch_set_mirror_x,
    .get_mirror_x = st77922_touch_get_mirror_x,
    .set_mirror_y = st77922_touch_set_mirror_y,
    .get_mirror_y = st77922_touch_get_mirror_y,
};

Driver st77922_touch_driver = {
    .name = "st77922-touch",
    .compatible = (const char*[]) { "sitronix,st77922-touch", nullptr },
    .start_device = start,
    .stop_device = stop,
    .probe = nullptr,
    .api = &st77922_touch_pointer_api,
    .device_type = &POINTER_TYPE,
    .internal = nullptr
};
