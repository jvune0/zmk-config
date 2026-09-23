/*
 * Battery level indicator on the RGB underglow LEDs.
 *
 * - &bat_ind shows the battery level of each half on its own LEDs for
 *   CONFIG_ZMK_BATTERY_LED_DURATION_MS: the number of lit LEDs is proportional
 *   to the charge, the color is green / yellow / red.
 * - When the battery drops to CONFIG_ZMK_BATTERY_LED_WARN_LEVEL (and then every
 *   CONFIG_ZMK_BATTERY_LED_WARN_STEP percent lower) the same picture blinks red.
 *
 * While the indicator is shown the regular underglow is switched off, and its
 * previous state is restored afterwards.
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
#include <drivers/ext_power.h>

#include <zmk/battery.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/rgb_underglow.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif
#include <zmk/workqueue.h>

LOG_MODULE_REGISTER(battery_led, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

// Time for the LED power rail to come up after EXT_POWER is enabled.
#define POWER_UP_DELAY K_MSEC(100)
#define BLINK_PERIOD K_MSEC(300)
// Battery events during boot are postponed until the underglow settings are loaded.
#define BOOT_GRACE_MS 10000

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#else
static const struct device *const ext_power = NULL;
#endif

static struct led_rgb pixels[STRIP_NUM_PIXELS];

// All of the state below is only touched from the low priority work queue,
// the same queue the underglow driver uses to update the strip.
static bool active;
static bool warn_mode;
static bool restore_underglow;
static bool restore_ext_power_off;
static int frame;

static atomic_t requested_warn;

static void frame_work_cb(struct k_work *work);
static void start_work_cb(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(frame_work, frame_work_cb);
static K_WORK_DEFINE(start_work, start_work_cb);

// Channel value (0-255) for the indicator.
static uint8_t indicator_brightness(void) {
#if CONFIG_ZMK_BATTERY_LED_BRIGHTNESS > 0
    int percent = CONFIG_ZMK_BATTERY_LED_BRIGHTNESS;
#else
    // Same scaling the underglow driver applies, so RGB_BRI / RGB_BRD adjust the indicator too.
    int percent = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
                  (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
                      zmk_rgb_underglow_calc_brt(0).b / 100;
#endif
    // Never go fully dark, otherwise the indicator would be invisible.
    return MAX(percent * 255 / 100, 1);
}

static struct led_rgb level_color(uint8_t level) {
    uint8_t v = indicator_brightness();

    if (level > CONFIG_ZMK_BATTERY_LED_LEVEL_HIGH) {
        return (struct led_rgb){.r = 0, .g = v, .b = 0};
    }
    if (level > CONFIG_ZMK_BATTERY_LED_LEVEL_LOW) {
        // Green WS2812 dies are brighter, tone them down to get a proper yellow.
        return (struct led_rgb){.r = v, .g = MAX(v * 2 / 3, 1), .b = 0};
    }
    return (struct led_rgb){.r = v, .g = 0, .b = 0};
}

static void draw(uint8_t level, bool lit) {
    struct led_rgb color = level_color(level);
    int count = lit ? DIV_ROUND_UP(MIN(level, 100) * STRIP_NUM_PIXELS, 100) : 0;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = i < count ? color : (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }

    int err = led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }
}

static void finish(void) {
    if (restore_underglow) {
        zmk_rgb_underglow_on();
    } else {
        draw(0, false);
        if (restore_ext_power_off && ext_power != NULL) {
            ext_power_disable(ext_power);
        }
    }

    active = false;
}

static void frame_work_cb(struct k_work *work) {
    uint8_t level = zmk_battery_state_of_charge();

    if (warn_mode) {
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

    if (!active) {
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

        active = true;
    }

    warn_mode = atomic_clear(&requested_warn);
    frame = 0;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &frame_work, POWER_UP_DELAY);
}

static void battery_led_indicator_start(bool warn) {
    if (warn) {
        atomic_set(&requested_warn, 1);
    }
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
        battery_led_indicator_start(true);
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
    battery_led_indicator_start(false);
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
