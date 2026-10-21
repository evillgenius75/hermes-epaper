#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Shared input-event vocabulary (docs/controls.md). Every input source —
 * knob/buttons, on-screen keyboard, BLE keyboard, voice — produces these;
 * screens consume them without knowing the source. */
typedef enum {
    EV_SCROLL_UP,
    EV_SCROLL_DOWN,
    EV_SELECT,       /* knob push / Enter */
    EV_BACK,         /* knob long-push / Esc */
    EV_HOME,         /* PWR press */
    EV_POWER_MENU,   /* PWR hold */
    EV_PTT_START,    /* BOOT hold start */
    EV_PTT_STOP,     /* BOOT release */
    EV_TEXT,         /* composed text ready (see text field) */
    EV_NET_ITEM,     /* inbox item arrived (see client.h) */
    EV_WAKE_TICK,    /* periodic wake/poll tick */
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    char text[1024]; /* EV_TEXT payload */
} app_event_t;
