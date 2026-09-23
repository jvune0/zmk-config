/*
 * Temporarily takes the LEDs over from the underglow for the indicators (battery level,
 * LED scan) and restores the underglow afterwards.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>

#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include "led_indicators.h"

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
static const struct device *const ext_power = NULL;
#endif

// Only touched from the low priority work queue, the same queue the underglow renders on.
static int users;
static bool restore_underglow;
static bool restore_ext_power_off;

void led_takeover_begin(void) {
    if (users++ > 0) {
        return;
    }

    bool on = false;
    zmk_rgb_underglow_get_state(&on);
    restore_underglow = on;
    if (on) {
        // Stops the underglow animation so it doesn't overwrite the indicator.
        zmk_rgb_underglow_off();
    }

    restore_ext_power_off = false;
    if (ext_power != NULL && ext_power_get(ext_power) <= 0) {
        ext_power_enable(ext_power);
        restore_ext_power_off = true;
    }
}

void led_takeover_end(void) {
    if (users == 0 || --users > 0) {
        return;
    }

    if (restore_underglow) {
        zmk_rgb_underglow_on();
        return;
    }

    static struct led_rgb black[STRIP_NUM_PIXELS];
    led_strip_update_rgb(strip, black, STRIP_NUM_PIXELS);

    bool keep_power = false;
#if IS_ENABLED(CONFIG_ZMK_LED_STRIP_INDICATORS)
    // The Caps Lock LED still needs the power rail.
    keep_power = led_strip_indicators_need_power();
#endif
    if (restore_ext_power_off && ext_power != NULL && !keep_power) {
        ext_power_disable(ext_power);
    }
}

bool led_takeover_active(void) { return users > 0; }
