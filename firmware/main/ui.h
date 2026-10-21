#pragma once

#include "app.h"
#include "client.h"

/* Screens and rendering. E-ink rules: partial refresh while interacting,
 * one full refresh when content changes (new message / prompt / screen
 * switch). Rendering is driven from app_main's event loop only.
 *
 * Panel (docs/hardware.md): SSD1677 on SPI3 — DC GPIO9, CS GPIO10,
 * SCK GPIO11, MOSI GPIO12, RST GPIO46, BUSY GPIO3 (busy level 1).
 * Framebuffer 48,000 bytes in PSRAM (+ a previous-frame copy for
 * changed-region diffs). Proven refresh modes to implement behind
 * ui_flush(): full base, fast base (OTP waveform), partial full-screen,
 * changed-region; count partials and force a base refresh periodically.
 * The driver itself is epd.c; what remains TODO here is the renderer. */

typedef enum {
    SCREEN_CHAT,       /* transcript + status line */
    SCREEN_COMPOSE,    /* on-screen keyboard */
    SCREEN_PROMPT,     /* approval / clarify choice list */
    SCREEN_COMMANDS,   /* slash-command shortcuts */
    SCREEN_SETTINGS,   /* Wi-Fi, server, Bluetooth pairing, about */
    SCREEN_POWER,      /* sleep / restart / power off / Wi-Fi off */
    SCREEN_LISTENING,  /* recording frame */
} screen_t;

void ui_init(void);
void ui_show(screen_t screen);
screen_t ui_current(void);

/* Feed one event to the active screen. Returns true if it needs a flush. */
bool ui_handle_event(const app_event_t *ev);

/* Transcript + inbox rendering. */
void ui_render_history_json(const char *json);
void ui_render_inbox_item(const inbox_item_t *item);

/* Status line: agent_state is "idle" | "working"; battery is 0-100 or -1. */
void ui_set_status(const char *agent_state, int battery_pct);

void ui_flush(bool full);

/* Display-sleep stage: blank the panel and put it to sleep (epd). */
void ui_display_sleep(void);
