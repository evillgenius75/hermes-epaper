#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "audio.h"
#include "ble_hid.h"
#include "board.h"
#include "client.h"
#include "controls.h"
#include "ui.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";
static QueueHandle_t s_events;
static long s_last_seq;
static volatile bool s_net_busy; /* a protocol request is in flight */

/* ---- Wi-Fi ----------------------------------------------------------------- */

static void wifi_connect(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, CONFIG_EPAPER_WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, CONFIG_EPAPER_WIFI_PASSWORD, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    esp_wifi_connect();
    /* TODO: wait on the IP_EVENT_STA_GOT_IP event group instead of a delay. */
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* ---- network task: long-poll the inbox while awake ------------------------- */

static void net_task(void *arg)
{
    inbox_item_t items[8];
    for (;;) {
        int count = 0;
        s_net_busy = true;
        esp_err_t err = client_poll_inbox(s_last_seq, 50, items, 8, &count);
        s_net_busy = false;
        if (err == ESP_OK) {
            for (int i = 0; i < count; i++) {
                ui_render_inbox_item(&items[i]);
                if (items[i].seq > s_last_seq) s_last_seq = items[i].seq;
            }
            client_free_items(items, count);
            if (count > 0) client_ack(s_last_seq); /* ack after render */
        } else {
            vTaskDelay(pdMS_TO_TICKS(5000)); /* back off on errors */
        }
    }
}

/* ---- sleep: staged light sleep (proven model — docs/hardware.md) ------------
 *
 * Stage 1 (display sleep): panel refreshed to blank + panel sleep; the SoC
 * stays awake and motion/input restores it with a full refresh.
 * Stage 2 (light sleep): esp_light_sleep_start(). Wake sources are BOOT
 * (GPIO0) and the PMIC IRQ (GPIO38, i.e. the PWR key), both low-level GPIO
 * wake, plus a timer wake for periodic inbox checks. Deep sleep is NOT
 * used: light sleep keeps RAM/state and the AXP2101 holds the rails.
 *
 * Two rules from the reference implementation are load-bearing:
 *  - suspend the button polling timer across light sleep (missed esp_timer
 *    ticks replay in a burst on wake and fake long-presses);
 *  - the press that wakes the device is consumed as wake-only, never as an
 *    app action (handled in controls.c for BOOT, and by the PMIC path for
 *    PWR — controls_notify_power_key is not called for wake presses).
 * -------------------------------------------------------------------------- */

static void enter_light_sleep(void)
{
    if (s_net_busy) {
        ESP_LOGI(TAG, "light sleep deferred: request in flight (blocker)");
        return;
    }
    ESP_LOGI(TAG, "entering light sleep");

    controls_suspend();
    ui_display_sleep();

    gpio_wakeup_enable(BOARD_BTN_BOOT_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BOARD_PMIC_IRQ_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup(
        (uint64_t)CONFIG_EPAPER_WAKE_INTERVAL_S * 1000000ULL);

    esp_light_sleep_start(); /* returns on wake */

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "woke: cause=%d", (int)cause);
    gpio_wakeup_disable(BOARD_BTN_BOOT_GPIO);
    gpio_wakeup_disable(BOARD_PMIC_IRQ_GPIO);
    controls_resume();
    /* TODO: if cause == ESP_SLEEP_WAKEUP_TIMER, do one inbox poll cycle
     * and re-enter sleep when nothing arrived and no input occurred. */
    ui_flush(true); /* wake always restores with a forced full refresh */
}

/* ---- app --------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_events = xQueueCreate(16, sizeof(app_event_t));
    s_last_seq = client_last_seq_load();

    ui_init();
    wifi_connect();

    /* Boot sync: server time -> RTC, then paint recent history. */
    long server_time = 0;
    char agent_state[16] = "idle";
    if (client_status(&server_time, agent_state, sizeof(agent_state)) == ESP_OK &&
        server_time > 0) {
        struct timeval tv = { .tv_sec = server_time };
        settimeofday(&tv, NULL);
    }
    ui_set_status(agent_state, -1);
    ui_show(SCREEN_CHAT);

    controls_start(s_events);
    ble_hid_boot(s_events); /* no-op unless a keyboard is bonded */
    xTaskCreate(net_task, "net", 8192, NULL, 4, NULL);

    int idle_s = 0;
    bool display_sleeping = false;
    for (;;) {
        app_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
            idle_s++;
            /* TODO(imu): idle here is wall-clock; the proven model keys
             * both stages off QMI8658 no-motion detection instead. */
            if (!display_sleeping &&
                idle_s >= CONFIG_EPAPER_DISPLAY_SLEEP_S) {
                display_sleeping = true;
                ESP_LOGI(TAG, "display sleep (TODO(hw): blank + panel sleep)");
            }
            if (CONFIG_EPAPER_IDLE_SLEEP_S > 0 &&
                idle_s >= CONFIG_EPAPER_IDLE_SLEEP_S) {
                enter_light_sleep();
                idle_s = 0;
                display_sleeping = false;
            }
            continue;
        }
        idle_s = 0;
        display_sleeping = false;

        switch (ev.type) {
        case EV_PTT_START:
            audio_record_start();
            ui_handle_event(&ev);
            break;
        case EV_PTT_STOP: {
            void *wav = NULL;
            size_t len = 0;
            if (audio_record_stop(&wav, &len) == ESP_OK) {
                client_send_voice(wav, len);
                free(wav);
            }
            ui_show(SCREEN_CHAT);
            break;
        }
        case EV_TEXT:
            client_send_text(ev.text);
            ui_show(SCREEN_CHAT);
            break;
        default:
            ui_handle_event(&ev);
            break;
        }
    }
}
