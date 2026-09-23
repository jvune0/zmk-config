/*
 * &led_scan lights the LEDs of the half it is pressed on one by one, from the first to the
 * last in the chain, to find out which LED sits under which key. The color changes every
 * 5 LEDs (red, green, blue, yellow, magenta), so the index is 5 * color + position in the
 * color group. Pressing it again stops the scan.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_led_scan

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/workqueue.h>

#include "led_indicators.h"

LOG_MODULE_REGISTER(led_scan, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)
#define GROUP_SIZE 5

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

// Only touched from the low priority work queue.
static struct led_rgb pixels[STRIP_NUM_PIXELS];
static bool running;
static int scan_index;

static struct led_rgb group_color(int i) {
    uint8_t v = indicator_channel_value(0);
    uint8_t y = MAX(v * 2 / 3, 1);

    switch ((i / GROUP_SIZE) % 5) {
    case 0:
        return (struct led_rgb){.r = v, .g = 0, .b = 0};
    case 1:
        return (struct led_rgb){.r = 0, .g = v, .b = 0};
    case 2:
        return (struct led_rgb){.r = 0, .g = 0, .b = v};
    case 3:
        return (struct led_rgb){.r = v, .g = y, .b = 0};
    default:
        return (struct led_rgb){.r = v, .g = 0, .b = v};
    }
}

static void step_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(step_work, step_work_cb);

static void step_work_cb(struct k_work *work) {
    if (!running) {
        return;
    }

    if (scan_index >= STRIP_NUM_PIXELS) {
        running = false;
        led_takeover_end();
        return;
    }

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = i == scan_index ? group_color(i) : (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }
    led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    LOG_INF("LED %d", scan_index);

    scan_index++;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &step_work,
                                K_MSEC(CONFIG_ZMK_LED_SCAN_DELAY_MS));
}

static void toggle_work_cb(struct k_work *work) {
    if (running) {
        running = false;
        k_work_cancel_delayable(&step_work);
        led_takeover_end();
        return;
    }

    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip is not ready");
        return;
    }

    led_takeover_begin();
    running = true;
    scan_index = 0;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &step_work,
                                LED_TAKEOVER_POWER_UP_DELAY);
}

static K_WORK_DEFINE(toggle_work, toggle_work_cb);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &toggle_work);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_led_scan_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    // Runs on the half the key is on.
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_led_scan_driver_api);
