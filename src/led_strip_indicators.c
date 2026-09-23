/*
 * LED strip proxy that shows the Caps Lock state on one LED.
 *
 * The zmk,underglow chosen node points at this device instead of the real strip. Every frame
 * the underglow (or the battery indicator) renders is passed through to the real strip, with
 * the Caps Lock LED painted on top while Caps Lock is on. When the underglow is off the LED
 * power rail is kept on for as long as the Caps Lock LED is lit.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_led_strip_indicators

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#include <drivers/ext_power.h>

#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

#include "led_indicators.h"

LOG_MODULE_REGISTER(led_strip_indicators, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Exactly one zmk,led-strip-indicators node is supported");

#define NODE DT_DRV_INST(0)
#define NUM_PIXELS DT_PROP(NODE, chain_length)
#define CAPS_LOCK_LED DT_PROP(NODE, caps_lock_led)

BUILD_ASSERT(NUM_PIXELS == DT_PROP(DT_PHANDLE(NODE, led_strip), chain_length),
             "chain-length must match the real LED strip");
BUILD_ASSERT(CAPS_LOCK_LED < NUM_PIXELS, "caps-lock-led is out of range");
BUILD_ASSERT(DT_PROP_LEN(NODE, caps_lock_color) == 3, "caps-lock-color must be <R G B>");

// Bit of Caps Lock in the HID LED output report.
#define HID_INDICATOR_CAPS_LOCK BIT(1)

// Time for the LED power rail to come up after EXT_POWER is enabled.
#define POWER_UP_DELAY_MS 50

static const struct device *const strip = DEVICE_DT_GET(DT_PHANDLE(NODE, led_strip));

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER) &&                                            \
    DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
static const struct device *const ext_power = NULL;
#endif

static const uint8_t caps_lock_color[] = DT_PROP(NODE, caps_lock_color);

// Everything below is only touched from the low priority work queue, which is where the
// underglow and the battery indicator render their frames.
static struct led_rgb frame[NUM_PIXELS];
static struct led_rgb out[NUM_PIXELS];
static bool caps_lock_on;

static atomic_t caps_lock_requested;

bool led_strip_indicators_need_power(void) { return caps_lock_on; }

static uint8_t scale(uint8_t channel) {
    if (channel == 0) {
        return 0;
    }
    return MAX(channel * indicator_channel_value(CONFIG_ZMK_CAPS_LOCK_LED_BRIGHTNESS) / 255, 1);
}

static void ensure_power(void) {
    if (ext_power != NULL && ext_power_get(ext_power) <= 0) {
        ext_power_enable(ext_power);
        k_msleep(POWER_UP_DELAY_MS);
    }
}

static int flush(void) {
    memcpy(out, frame, sizeof(out));

    if (caps_lock_on) {
        ensure_power();
        out[CAPS_LOCK_LED] = (struct led_rgb){
            .r = scale(caps_lock_color[0]),
            .g = scale(caps_lock_color[1]),
            .b = scale(caps_lock_color[2]),
        };
    }

    return led_strip_update_rgb(strip, out, NUM_PIXELS);
}

static int proxy_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num_pixels) {
    memcpy(frame, pixels, MIN(num_pixels, NUM_PIXELS) * sizeof(struct led_rgb));
    return flush();
}

static int proxy_update_channels(const struct device *dev, uint8_t *channels,
                                 size_t num_channels) {
    return -ENOTSUP;
}

static const struct led_strip_driver_api proxy_api = {
    .update_rgb = proxy_update_rgb,
    .update_channels = proxy_update_channels,
};

static void refresh_work_cb(struct k_work *work) {
    bool on = atomic_get(&caps_lock_requested);
    if (on == caps_lock_on) {
        return;
    }
    caps_lock_on = on;

    bool underglow_on = false;
    zmk_rgb_underglow_get_state(&underglow_on);

    if (!caps_lock_on) {
        // Clear the LED, the rest of the frame stays as the underglow left it.
        frame[CAPS_LOCK_LED] = (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }

    int err = flush();
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }

    // The power rail was only kept on for the Caps Lock LED.
    bool taken_over = false;
#if IS_ENABLED(CONFIG_ZMK_LED_TAKEOVER)
    taken_over = led_takeover_active();
#endif
    if (!caps_lock_on && !underglow_on && !taken_over && ext_power != NULL &&
        ext_power_get(ext_power) > 0) {
        ext_power_disable(ext_power);
    }
}

static K_WORK_DEFINE(refresh_work, refresh_work_cb);

static int hid_indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    atomic_set(&caps_lock_requested, (ev->indicators & HID_INDICATOR_CAPS_LOCK) ? 1 : 0);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &refresh_work);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_strip_indicators, hid_indicators_listener);
ZMK_SUBSCRIPTION(led_strip_indicators, zmk_hid_indicators_changed);

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_LED_STRIP_INIT_PRIORITY, &proxy_api);
