/*
 * Helpers shared by the LED indicators (battery level, Caps Lock).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/rgb_underglow.h>

// Brightness in percent of full scale the underglow currently renders with, using the same
// scaling as the underglow driver, so RGB_BRI / RGB_BRD adjust the indicators too.
static inline int underglow_brightness_percent(void) {
    return CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
           (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
               zmk_rgb_underglow_calc_brt(0).b / 100;
}

// True while the battery level is being shown. Only valid on the low priority work queue.
bool battery_led_indicator_is_active(void);

// True while an indicator LED (Caps Lock) is lit and needs the LED power rail.
// Only valid on the low priority work queue.
bool led_strip_indicators_need_power(void);
