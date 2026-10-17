// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tactility/bindings/bindings.h>
#include <drivers/st77922_touch.h>

// The devicetree compiler derives the config typedef name from the compatible string's suffix,
// with hyphens replaced by underscores ("sitronix,st77922-touch" -> st77922_touch_config_dt).
DEFINE_DEVICETREE(st77922_touch, struct St77922TouchConfig)
