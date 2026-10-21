#include "ui.h"

#include <string.h>
#include "epd.h"
#include "esp_log.h"

static const char *TAG = "ui";
static screen_t s_screen = SCREEN_CHAT;
static char s_agent_state[16] = "idle";
static int s_battery = -1;
static int s_partial_count;

/* Panel driver is epd.c (SSD1677). Still TODO: the text renderer — font
 * rasterization into the epd framebuffer. The reference project's UI work
 * proves a full widget toolkit is feasible on this panel; v1 needs only a
 * bitmap font + wrapped text + simple lists. */

#define UI_MAX_PARTIALS 5 /* force a base refresh after this many partials */

void ui_init(void)
{
    if (epd_init() == ESP_OK) {
        epd_clear(true);
    } else {
        ESP_LOGE(TAG, "panel init failed");
    }
    s_screen = SCREEN_CHAT;
}

void ui_show(screen_t screen)
{
    s_screen = screen;
    ESP_LOGI(TAG, "show screen %d", screen);
    ui_flush(true);
}

screen_t ui_current(void) { return s_screen; }

bool ui_handle_event(const app_event_t *ev)
{
    switch (ev->type) {
    case EV_HOME:
        ui_show(SCREEN_CHAT);
        return true;
    case EV_POWER_MENU:
        ui_show(SCREEN_POWER);
        return true;
    case EV_PTT_START:
        ui_show(SCREEN_LISTENING);
        return true;
    case EV_BACK:
        if (s_screen != SCREEN_CHAT) {
            ui_show(SCREEN_CHAT);
            return true;
        }
        return false;
    case EV_TEXT:
        /* Compose screens consume text via their own state; main.c routes
         * EV_TEXT sends. Rendering of the composer is renderer TODO. */
        return false;
    default:
        /* TODO: per-screen navigation (scroll offsets, OSK focus, prompt
         * choice selection) lives here as screens are built out. */
        return false;
    }
}

void ui_render_history_json(const char *json)
{
    (void)json;
    ESP_LOGI(TAG, "render history (renderer TODO)");
}

void ui_render_inbox_item(const inbox_item_t *item)
{
    ESP_LOGI(TAG, "render item seq=%ld kind=%d", item->seq, item->kind);
    if (item->kind == ITEM_PROMPT) {
        ui_show(SCREEN_PROMPT);
    }
    ui_flush(true);
}

void ui_set_status(const char *agent_state, int battery_pct)
{
    strncpy(s_agent_state, agent_state, sizeof(s_agent_state) - 1);
    s_battery = battery_pct;
}

void ui_flush(bool full)
{
    if (full || s_partial_count >= UI_MAX_PARTIALS) {
        epd_refresh_full();
        s_partial_count = 0;
    } else {
        epd_refresh_partial();
        s_partial_count++;
    }
    ESP_LOGI(TAG, "flush (%s) state=%s batt=%d",
             full ? "full" : "partial", s_agent_state, s_battery);
}

void ui_display_sleep(void)
{
    /* Proven sequence: refresh to blank, wait, then panel sleep. */
    epd_clear(true);
    epd_refresh_full();
    epd_sleep();
}
