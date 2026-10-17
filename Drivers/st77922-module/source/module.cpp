// SPDX-License-Identifier: Apache-2.0
#include <tactility/driver.h>
#include <tactility/module.h>

extern "C" {

extern Driver st77922_driver;
extern Driver st77922_touch_driver;

static Driver* const st77922_drivers[] = {
    &st77922_driver,
    &st77922_touch_driver,
    nullptr
};

Module st77922_module = {
    .name = "st77922",
    .start = nullptr,
    .stop = nullptr,
    .drivers = st77922_drivers,
    .symbols = nullptr,
    .internal = nullptr
};

} // extern "C"
