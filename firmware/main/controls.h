#pragma once

#include "app.h"
#include "board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Physical controls (docs/controls.md + docs/hardware.md).
 *
 * The board has a rocker + BOOT + PWR — no rotary encoder:
 *   rocker UP   (GPIO4)  scroll up;   hold = repeat-scroll (after 350 ms)
 *   rocker DOWN (GPIO6)  scroll down; hold = repeat-scroll
 *   rocker FN   (GPIO5)  click = SELECT, long-press = BACK
 *   BOOT        (GPIO0)  hold = push-to-talk (PTT_START on press,
 *                        PTT_STOP on release)
 *   PWR         (AXP2101) short = HOME, long = POWER_MENU; delivered via
 *                        controls_notify_power_key() from the PMIC driver
 *
 * Implementation note: the proven stack for these buttons is Espressif's
 * iot_button component (espressif/button), which provides exactly the
 * click / repeat / long-press classification this mapping needs. This
 * module wraps it and translates its events into app_event_t. If iot_button
 * is in use, Suspend()/Resume() around light sleep are MANDATORY — its
 * esp_timer replays missed ticks after sleep and fakes long-presses
 * (see docs/hardware.md, Sleep). */

#define CONTROLS_FN_LONG_PRESS_MS 600

/* Posts app events to `out`. */
void controls_start(QueueHandle_t out);

/* Call from the AXP2101 IRQ callback path (task context, not ISR). */
void controls_notify_power_key(bool long_press);

/* Light-sleep integration: stop button polling before sleeping, resume
 * after wake (re-anchors the polling timer). */
void controls_suspend(void);
void controls_resume(void);
