/*
 * Helpers shared by the LED indicators (battery level, Caps Lock, LED scan).
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/rgb_underglow.h>

// Brightness in percent of full scale the underglow currently renders with, using the same
// scaling as the underglow driver, so RGB_BRI / RGB_BRD adjust the indicators too.
static inline int underglow_brightness_percent(void) {
    return CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
           (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
               zmk_rgb_underglow_calc_brt(0).b / 100;
}

// Time for the LED power rail to come up after led_takeover_begin() enabled it.
#define LED_TAKEOVER_POWER_UP_DELAY K_MSEC(100)

// Temporarily take the LEDs over from the underglow: stops the underglow animation and makes
// sure the LEDs are powered. Calls nest, the last led_takeover_end() restores the underglow.
// Only call these from the low priority work queue.
void led_takeover_begin(void);
void led_takeover_end(void);
bool led_takeover_active(void);

// Brightness (0-255 channel value) for the indicators; percent 0 follows the underglow.
static inline uint8_t indicator_channel_value(int percent) {
    if (percent <= 0) {
        percent = underglow_brightness_percent();
    }
    // Never go fully dark, otherwise the indicator would be invisible.
    return MAX(percent * 255 / 100, 1);
}

// True while an indicator LED (Caps Lock) is lit and needs the LED power rail.
// Only valid on the low priority work queue.
bool led_strip_indicators_need_power(void);
