/*
 * Battery level indicator on the RGB underglow LEDs.
 *
 * - &bat_ind shows the battery level of each half on its own LEDs for
 *   CONFIG_ZMK_BATTERY_LED_DURATION_MS: the number of lit LEDs is proportional
 *   to the charge, the color is green / yellow / red.
 * - When the battery drops to CONFIG_ZMK_BATTERY_LED_WARN_LEVEL (and then every
 *   CONFIG_ZMK_BATTERY_LED_WARN_STEP percent lower) the same picture blinks red.
 * - &bat_test plays the indicator from 100% down to 0%, one percent every
 *   CONFIG_ZMK_BATTERY_LED_TEST_STEP_MS, to preview how it looks. Pressing it again stops it.
 *
 * While the indicator is shown the regular underglow is switched off, and its
 * previous state is restored afterwards (see led_takeover.c).
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_battery_indicator

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/battery.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/rgb_underglow.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#include <zmk/workqueue.h>

#include "led_indicators.h"

LOG_MODULE_REGISTER(battery_led, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

#define BLINK_PERIOD K_MSEC(300)
// Battery events during boot are postponed until the underglow settings are loaded.
#define BOOT_GRACE_MS 10000

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb pixels[STRIP_NUM_PIXELS];

// All of the state below is only touched from the low priority work queue.
enum mode {
    MODE_SHOW,
    MODE_WARN,
    MODE_TEST,
};

static bool active;
static enum mode mode;
static int frame;

static atomic_t requested_mode;

static void frame_work_cb(struct k_work *work);
static void start_work_cb(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(frame_work, frame_work_cb);
static K_WORK_DEFINE(start_work, start_work_cb);

static struct led_rgb level_color(uint8_t level) {
    uint8_t v = indicator_channel_value(CONFIG_ZMK_BATTERY_LED_BRIGHTNESS);

    if (level > CONFIG_ZMK_BATTERY_LED_LEVEL_HIGH) {
        return (struct led_rgb){.r = 0, .g = v, .b = 0};
    }
    if (level > CONFIG_ZMK_BATTERY_LED_LEVEL_LOW) {
        // Green WS2812 dies are brighter, tone them down to get a proper yellow.
        return (struct led_rgb){.r = v, .g = MAX(v * 2 / 3, 1), .b = 0};
    }
    return (struct led_rgb){.r = v, .g = 0, .b = 0};
}

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_led_display_order)
#define DISPLAY_ORDER_NODE DT_INST(0, zmk_led_display_order)
BUILD_ASSERT(DT_PROP_LEN(DISPLAY_ORDER_NODE, order) == STRIP_NUM_PIXELS,
             "led-display-order must list every LED of the strip");
static const uint8_t display_order[] = DT_PROP(DISPLAY_ORDER_NODE, order);
#define DISPLAY_LED(i) display_order[i]
#else
// No display order given, fill the LEDs in chain order.
#define DISPLAY_LED(i) (i)
#endif

static void draw(uint8_t level, bool lit) {
    struct led_rgb color = level_color(level);
    int count = lit ? DIV_ROUND_UP(MIN(level, 100) * STRIP_NUM_PIXELS, 100) : 0;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[DISPLAY_LED(i)] = i < count ? color : (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }

    int err = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }
}

static void finish(void) {
    led_takeover_end();
    active = false;
}

static void frame_work_cb(struct k_work *work) {
    if (mode == MODE_TEST) {
        // Frame N shows 100 - N percent; after 0% has been shown for one step we're done.
        int level = 100 - frame;
        if (level < 0) {
            finish();
            return;
        }
        draw(level, true);
        frame++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &frame_work,
                                    K_MSEC(CONFIG_ZMK_BATTERY_LED_TEST_STEP_MS));
        return;
    }

    uint8_t level = zmk_battery_state_of_charge();

    if (mode == MODE_WARN) {
        // Even frames are "on", odd frames are "off".
        if (frame >= CONFIG_ZMK_BATTERY_LED_WARN_BLINKS * 2) {
            finish();
            return;
        }
        draw(level, frame % 2 == 0);
        frame++;
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &frame_work, BLINK_PERIOD);
        return;
    }

    if (frame > 0) {
        finish();
        return;
    }
    draw(level, true);
    frame++;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &frame_work,
                                K_MSEC(CONFIG_ZMK_BATTERY_LED_DURATION_MS));
}

static void start_work_cb(struct k_work *work) {
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip is not ready");
        return;
    }

    enum mode requested = atomic_get(&requested_mode);

    if (active && mode == MODE_TEST && requested == MODE_TEST) {
        // Second press of &bat_test stops the test.
        k_work_cancel_delayable(&frame_work);
        finish();
        return;
    }

    if (!active) {
        led_takeover_begin();
        active = true;
    }

    mode = requested;
    frame = 0;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &frame_work,
                                LED_TAKEOVER_POWER_UP_DELAY);
}

static void battery_led_indicator_start(enum mode requested) {
    atomic_set(&requested_mode, requested);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &start_work);
}

#if CONFIG_ZMK_BATTERY_LED_WARN_LEVEL > 0

// Level at which the last warning was shown, or -1 if none since the battery was above the
// threshold.
static int last_warned_level = -1;

static void check_low_battery(uint8_t level) {
    if (level == 0) {
        // Not measured yet.
        return;
    }

    if (level > CONFIG_ZMK_BATTERY_LED_WARN_LEVEL) {
        // Small hysteresis so the warning doesn't repeat when the level jitters around the
        // threshold.
        if (level > CONFIG_ZMK_BATTERY_LED_WARN_LEVEL + CONFIG_ZMK_BATTERY_LED_WARN_STEP) {
            last_warned_level = -1;
        }
        return;
    }

#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        // Charging, no point in warning.
        return;
    }
#endif

    if (last_warned_level < 0 || level + CONFIG_ZMK_BATTERY_LED_WARN_STEP <= last_warned_level) {
        last_warned_level = level;
        battery_led_indicator_start(MODE_WARN);
    }
}

static void boot_check_work_cb(struct k_work *work) {
    check_low_battery(zmk_battery_state_of_charge());
}

static K_WORK_DELAYABLE_DEFINE(boot_check_work, boot_check_work_cb);

static int battery_led_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int64_t uptime = k_uptime_get();
    if (uptime < BOOT_GRACE_MS) {
        k_work_reschedule(&boot_check_work, K_MSEC(BOOT_GRACE_MS - uptime));
        return ZMK_EV_EVENT_BUBBLE;
    }

    check_low_battery(ev->state_of_charge);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(battery_led_indicator, battery_led_listener);
ZMK_SUBSCRIPTION(battery_led_indicator, zmk_battery_state_changed);

#endif // CONFIG_ZMK_BATTERY_LED_WARN_LEVEL > 0

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    battery_led_indicator_start(MODE_SHOW);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_battery_indicator_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    // Runs on both halves, each one shows its own battery.
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_battery_indicator_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zmk_behavior_battery_test

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_test_binding_pressed(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    battery_led_indicator_start(MODE_TEST);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_test_binding_released(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_battery_test_driver_api = {
    .binding_pressed = on_test_binding_pressed,
    .binding_released = on_test_binding_released,
    // Runs on both halves, like &bat_ind.
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_battery_test_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
