# Firmware — hermes-epaper

ESP-IDF firmware for the Waveshare ESP32-S3-ePaper-3.97.

## Build & flash

```sh
# ESP-IDF v5.x in your environment
idf.py set-target esp32s3
idf.py menuconfig     # "Hermes ePaper": Wi-Fi, server URL, device token
idf.py build flash monitor
```

Configuration lives in Kconfig (`main/Kconfig.projbuild`): Wi-Fi credentials,
the adapter base URL, the device token (must match `EPAPER_DEVICE_TOKEN` on
the gateway), device id (blank = MAC-derived), wake interval, idle sleep.

## Layout

| File | Role |
| --- | --- |
| `main/main.c` | Boot, Wi-Fi, event loop, sleep cycle, net task |
| `main/controls.c/h` | Knob/BOOT/PWR input → event queue (docs/controls.md) |
| `main/client.c/h` | Device-protocol client (docs/protocol.md §4) |
| `main/audio.c/h` | ES8311 push-to-talk recording (WAV to PSRAM) |
| `main/ui.c/h` | Screens + e-paper rendering |
| `main/app.h` | Shared input-event vocabulary |

## Status

Working skeleton. All pin assignments and peripheral parameters are known
and proven — they come from the waveshare-airecorder project running on
this exact board; see `docs/hardware.md` for the full reference and
`main/board.h` for the defines. Hardware-independent logic (protocol
client, boot flow, staged light-sleep flow) is implemented. Remaining
`TODO(hw)` work is driver bring-up, not unknowns:

- SSD1677 driver: written (`epd.c`, from the datasheet — see its header
  for provenance); waveform constants flagged BENCH need confirmation at
  first hardware bring-up, and the text renderer is still TODO
- ES8311 bring-up via esp_codec_dev (`audio.c`)
- AXP2101 minimal driver: rails at boot, battery state, PKEY IRQ decoding
  wired to `controls_notify_power_key()`
- iot_button timer suspend/resume across light sleep (`controls.c`)
- QMI8658 no-motion polling to drive the sleep stages (`main.c`)
- BLE HID host: module written (`ble_hid.c`, lazy-init zero-idle-cost
  design); needs on-device validation — no precedent in the reference
  project
