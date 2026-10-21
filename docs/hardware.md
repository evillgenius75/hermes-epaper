# Hardware Reference — Waveshare ESP32-S3-ePaper-3.97

Everything below is **proven on the physical board** by the
[waveshare-airecorder](https://github.com/evillgenius75/waveshare-airecorder)
project (same author), whose firmware runs on this exact device. Where the
Waveshare wiki and the working firmware disagree, the firmware wins — those
cases are called out explicitly.

> ⚠️ **License note:** waveshare-airecorder is GPL-3.0; this project is MIT.
> The *facts* in this document (pins, addresses, timings) are free to use.
> Its driver code is not copied here — display/codec/button support is
> implemented against the same upstream Espressif components it uses
> (esp_codec_dev, espressif/button — both Apache-2.0), or written fresh.

## Board summary

| Item | Value |
| --- | --- |
| MCU module | ESP32-S3-WROOM-1-N16R8, 240 MHz |
| Flash / PSRAM | 16 MB / 8 MB octal PSRAM @ 80 MHz |
| Display | 3.97" e-paper, **SSD1677** controller, 800×480, SPI3 |
| Audio codec | ES8311 (I2S + I2C), NS4150B amp |
| PMIC | **AXP2101** — the wiki's "TG28" label is wrong; AXP2101 @ I2C 0x34 is what works |
| RTC | PCF85063 |
| IMU | QMI8658 (6-axis) |
| Temp/humidity | SHTC3 (on bus; no driver written yet) |
| Storage | MicroSD, SDMMC 4-bit |
| USB | Native USB-OTG on the Type-C connector |

## Controls — there is no rotary encoder

The physical controls are a **rocker** (up / down / middle-push) plus **BOOT**
and **PWR** buttons. All are momentary, active-low to GND:

| Control | GPIO | Notes |
| --- | --- | --- |
| Rocker UP | GPIO4 | Hold-to-repeat scrolling (repeat starts after 350 ms) |
| Rocker DOWN | GPIO6 | Same |
| Rocker middle (FN) | GPIO5 | Click = select; also double-click and long-press |
| BOOT (ACTION) | GPIO0 | Push-to-talk; also a light-sleep wake source. Strapping pin — must read high at reset |
| PWR | — (AXP2101) | Not a GPIO. The PMIC raises `PKEY_SHORT_IRQ` / `PKEY_LONG_IRQ`; MCU sees them via the PMIC IRQ line (GPIO38). If both latch, long wins |

Proven input stack: Espressif's `button` component (iot_button, v4.1.6)
producing press-down/up, repeat, single-click, double-click, long-press-start,
long-press-up events per button.

## E-paper (SSD1677, SPI3_HOST, write-only)

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| DC | 9 | | RST | 46 (strapping pin — care at reset) |
| CS | 10 | | BUSY | 3 (strapping pin; busy level = 1) |
| SCK | 11 | | MOSI | 12 |

- Framebuffer: 800×480 ÷ 8 = **48,000 bytes** (keep in PSRAM, plus a second
  buffer for changed-region diffing).
- Proven refresh repertoire: **full base** refresh, **fast base** refresh
  (OTP fast waveform — quicker, clears ghosting less), **partial full-screen**,
  and **changed-region** updates. Partial refreshes are counted; a base
  refresh is forced after N partials.
- Panel has no GPIO power enable — it runs from AXP2101 rails.
- Busy timeout 10 s; reset pulse 2 ms low / 50 ms high.

## Audio (ES8311 + NS4150B)

| Signal | GPIO | | Signal | GPIO |
| --- | --- | --- | --- | --- |
| MCLK | 13 | | DIN (mic) | 21 |
| BCLK | 14 | | DOUT (speaker) | 48 |
| WS/LRCK | 47 | | PA enable | 39 |

- Codec control: I2C **0x30** on the shared bus. Streaming: I2S0, full-duplex.
- **16 kHz, mono, 16-bit** in and out (rates must match for full-duplex) —
  exactly the WAV format the device protocol uses, no resampling.
- Driver: `esp_codec_dev` (~1.5.4). Output volume runs at full scale — the
  small speaker has no headroom to spare.

## Shared I2C bus (I2C_NUM_1, SDA GPIO41 / SCL GPIO42, 400 kHz)

| Device | Address | Interrupt |
| --- | --- | --- |
| ES8311 codec | 0x30 | — |
| AXP2101 PMIC | 0x34 | GPIO38 (also VBUS insert/remove) |
| PCF85063 RTC | 0x51 | GPIO45 (strapping pin) |
| QMI8658 IMU | 0x6B (fallback 0x6A) | INT2 GPIO40 |
| SHTC3 temp/humidity | 0x70 | — (no driver yet) |

## Power (AXP2101)

- Rails enabled at bring-up: DC1 + ALDO1/2/3, all 3300 mV. System power-down
  threshold 2800 mV.
- Charging: target 4.2 V, constant current 400 mA, precharge 75 mA,
  termination 25 mA; VBUS limit 4.36 V / 900 mA.
- Low battery: warn at 10 %, shutdown threshold 5 %.
- Power on is via the PMIC hardware key path; a **6 s PWR hold is a
  PMIC-enforced forced shutdown** — the app-level hold must stay well under
  that (the reference app uses ~1 s for its power menu).
- The PMIC holds all rails across light sleep by itself — no power-latch
  GPIO dance is needed.

## Sleep — light sleep, staged (proven model)

Deep sleep is **not** used. The proven model is:

1. **Display sleep** (default after 180 s of no motion): refresh panel to
   blank, wait for refresh, panel sleep. Motion or any interaction wakes it
   with a full refresh.
2. **Light sleep** (default after 1800 s of no motion): panel asleep, then
   `esp_light_sleep_start()`. RAM and state survive.
   - Wake sources: **GPIO0** and **GPIO38**, both low-level, via
     `gpio_wakeup_enable` + `esp_sleep_enable_gpio_wakeup()` (the light-sleep
     GPIO path, not EXT1). A timer wake can be armed alongside for periodic
     inbox checks.
   - **Wake-press suppression:** the press that wakes the device must be
     consumed as wake-only — for both BOOT and the PMIC power key — or it
     leaks into the app as a spurious action.
   - **iot_button gotcha:** its esp_timer replays every missed tick after a
     light sleep in a tight burst, instantly faking a long-press. Suspend the
     button timer before sleeping; resume after wake with the timer
     re-anchored.
- Inactivity is detected by polling the QMI8658 every 200 ms (no FIFO, no IMU
  interrupt): motion = axis-delta sum ≥ 60 mg or single axis ≥ 25 mg;
  stillness = sum ≤ 20 mg and single axis ≤ 8 mg, held 2 s to arm no-motion.
  Validated on-device (table vibration doesn't trip it; pickup wakes fast).
- Sleep blockers (pattern): recording, display refresh in flight, storage
  writes, setup AP mode, time sync. For this project add: HTTP request in
  flight, unresolved prompt on screen.

## MicroSD (SDMMC 4-bit)

CLK GPIO16, CMD GPIO17, D0 GPIO15, D1 GPIO7, D2 GPIO8, D3 GPIO18.

## Build configuration (proven values)

- Partitions: **OTA layout** — nvs 0x6000 @0x9000, otadata, phy_init,
  ota_0/ota_1 of 0x7E0000 each, coredump 0x20000. (The reference project
  treats this table as final for fielded devices.)
- PSRAM: `SPIRAM_USE_MALLOC` with `MALLOC_ALWAYSINTERNAL=2048`,
  `MALLOC_RESERVE_INTERNAL=98304`, `TRY_ALLOCATE_WIFI_LWIP`.
- TLS: `MBEDTLS_EXTERNAL_MEM_ALLOC` (crypto buffers in PSRAM — needed for
  HTTPS headroom).
- Flash QIO @ 80 MHz; bootloader app rollback enabled; coredump to flash;
  main task stack 8192.
- Managed components: `espressif/button` 4.1.6, `esp_codec_dev` ~1.5.4,
  `cjson` ^1.7.18. AXP2101 via XPowersLib (vendored there; port or write a
  minimal driver against the register map — only IRQs, rails, and battery
  state are needed here).

## Still open after this reference

- **BLE HID host** (keyboard pairing): the S3 is BLE-capable, but the
  reference project never enables Bluetooth — our BLE keyboard support has
  no proven on-device precedent yet. A NimBLE host module with a
  zero-idle-cost design (lazy init, pairing-only scanning) lives in
  `firmware/main/ble_hid.c`; it still needs on-device validation.
- **SHTC3 driver**: not written anywhere yet (only needed if we ever want
  temp/humidity).
- Everything server-side (Hermes prompt hook, voice media field) is
  unaffected by this document.
