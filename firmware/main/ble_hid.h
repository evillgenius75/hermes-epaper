#pragma once

#include <stdbool.h>
#include "app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* BLE HID keyboard host (NimBLE central + GATT client).
 *
 * Performance contract — BLE must never tax the device when unused:
 *  - The Bluetooth stack is NOT initialized at boot. ble_hid_boot() only
 *    starts it when a keyboard bond already exists in NVS. With no bond,
 *    BLE costs exactly zero: no tasks, no RAM, no radio.
 *  - Scanning happens ONLY while the Settings -> Bluetooth pairing screen
 *    is open. Leaving the screen stops the scan; if nothing bonded or
 *    connected, the stack is fully deinitialized again.
 *  - A connected keyboard uses a low-duty-cycle connection (30-50 ms
 *    interval, latency 4): a few tiny packets per keystroke. NimBLE's host
 *    task sleeps between events; report parsing is a table lookup.
 *  - Wi-Fi coexistence stays at the ESP-IDF default; HID traffic is far
 *    too small to disturb the inbox long-poll.
 *
 * Events posted to the app queue reuse the shared vocabulary: printable
 * characters arrive as EV_TEXT (one char each), Enter as EV_TEXT "\n",
 * Backspace as EV_TEXT "\b", Esc as EV_BACK. Screens interpret them, so a
 * BLE keyboard behaves exactly like the on-screen keyboard.
 *
 * Compiled in only with CONFIG_EPAPER_BLE_KEYBOARD; otherwise all entry
 * points are no-op stubs. */

void ble_hid_boot(QueueHandle_t out);

/* Pairing flow for the Settings screen. start begins scanning for HID
 * keyboards and connects to the first one selected by ble_hid_pair_index
 * (v1: the first found). stop ends pairing mode. */
bool ble_hid_start_pairing(void);
void ble_hid_stop_pairing(void);

bool ble_hid_connected(void);
bool ble_hid_bonded(void);

/* Remove the bond and shut BLE down. */
void ble_hid_forget(void);
