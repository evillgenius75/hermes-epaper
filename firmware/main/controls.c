#include "controls.h"

#include "esp_log.h"
#include "iot_button.h"

static const char *TAG = "controls";
static QueueHandle_t s_out;

/* Armed before light sleep: the BOOT press that wakes the device must not
 * start a recording. Cleared by the matching release (see controls.h). */
static volatile bool s_boot_wake_suppressed;

static void post(app_event_type_t type)
{
    app_event_t ev = { .type = type };
    xQueueSend(s_out, &ev, 0);
}

static void on_nav(button_handle_t btn, void *arg)
{
    button_event_t ev = iot_button_get_event(btn);
    app_event_type_t scroll = (app_event_type_t)(intptr_t)arg;
    if (ev == BUTTON_PRESS_DOWN || ev == BUTTON_PRESS_REPEAT) {
        post(scroll);
    }
}

static void on_fn(button_handle_t btn, void *arg)
{
    (void)arg;
    button_event_t ev = iot_button_get_event(btn);
    if (ev == BUTTON_SINGLE_CLICK) {
        post(EV_SELECT);
    } else if (ev == BUTTON_LONG_PRESS_START) {
        post(EV_BACK);
    }
}

static void on_boot(button_handle_t btn, void *arg)
{
    (void)arg;
    button_event_t ev = iot_button_get_event(btn);
    if (ev == BUTTON_PRESS_DOWN) {
        if (!s_boot_wake_suppressed) {
            post(EV_PTT_START);
        }
    } else if (ev == BUTTON_PRESS_UP) {
        if (s_boot_wake_suppressed) {
            s_boot_wake_suppressed = false; /* consumed as wake-only */
        } else {
            post(EV_PTT_STOP);
        }
    }
}

static button_handle_t make_button(gpio_num_t gpio, uint16_t long_press_ms)
{
    button_config_t cfg = {
        .long_press_time = long_press_ms,
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = 0,
    };
    button_handle_t handle = NULL;
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&cfg, &gpio_cfg, &handle));
    return handle;
}

void controls_start(QueueHandle_t out)
{
    s_out = out;

    /* Nav rocker: repeat after 350 ms hold (proven value). Long-press is
     * unused for nav, so give it a large threshold. */
    button_handle_t up = make_button(BOARD_BTN_UP_GPIO, 5000);
    iot_button_register_cb(up, BUTTON_PRESS_DOWN, NULL, on_nav,
                           (void *)(intptr_t)EV_SCROLL_UP);
    iot_button_register_cb(up, BUTTON_PRESS_REPEAT, NULL, on_nav,
                           (void *)(intptr_t)EV_SCROLL_UP);

    button_handle_t down = make_button(BOARD_BTN_DOWN_GPIO, 5000);
    iot_button_register_cb(down, BUTTON_PRESS_DOWN, NULL, on_nav,
                             (void *)(intptr_t)EV_SCROLL_DOWN);
    iot_button_register_cb(down, BUTTON_PRESS_REPEAT, NULL, on_nav,
                             (void *)(intptr_t)EV_SCROLL_DOWN);

    button_handle_t fn = make_button(BOARD_BTN_FN_GPIO, CONTROLS_FN_LONG_PRESS_MS);
    iot_button_register_cb(fn, BUTTON_SINGLE_CLICK, NULL, on_fn, NULL);
    iot_button_register_cb(fn, BUTTON_LONG_PRESS_START, NULL, on_fn, NULL);

    button_handle_t boot = make_button(BOARD_BTN_BOOT_GPIO, 5000);
    iot_button_register_cb(boot, BUTTON_PRESS_DOWN, NULL, on_boot, NULL);
    iot_button_register_cb(boot, BUTTON_PRESS_UP, NULL, on_boot, NULL);

    ESP_LOGI(TAG, "controls ready (rocker + FN + BOOT via iot_button)");
}

void controls_notify_power_key(bool long_press)
{
    /* TODO(power): the AXP2101 driver (XPowersLib port or minimal driver)
     * decodes PKEY_SHORT_IRQ / PKEY_LONG_IRQ in its IRQ task and calls
     * this. While display/light sleeping, main consumes the press as
     * wake-only instead (see device sleep design in docs/hardware.md). */
    post(long_press ? EV_POWER_MENU : EV_HOME);
}

void controls_suspend(void)
{
    /* TODO(sleep): stop the shared iot_button polling timer before
     * esp_light_sleep_start(). Mandatory — see controls.h. The reference
     * implementation (waveshare-airecorder button_service) stops/restarts
     * the component's esp_timer; do the equivalent for the pinned
     * espressif/button version in use. */
    ESP_LOGW(TAG, "controls_suspend: TODO(sleep) — stop iot_button timer");
}

void controls_resume(void)
{
    /* TODO(sleep): restart the polling timer so it re-anchors to now. */
    ESP_LOGW(TAG, "controls_resume: TODO(sleep) — restart iot_button timer");
}
