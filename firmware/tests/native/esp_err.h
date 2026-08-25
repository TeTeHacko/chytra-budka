/* Native-test shim for ESP-IDF's esp_err.h. Provides esp_err_t and the
 * common ESP_ERR_* / ESP_OK / ESP_FAIL values that firmware code uses.
 * Real (IDF-built) firmware never sees this file — only tests/native/
 * picks it up via -I. */
#pragma once

#include "esp_shim.h"
