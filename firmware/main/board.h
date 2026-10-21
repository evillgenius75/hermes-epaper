#pragma once

/* Board definitions — Waveshare ESP32-S3-ePaper-3.97.
 * Every value here is proven on the physical device by the
 * waveshare-airecorder project. See docs/hardware.md for the full
 * reference, including the cases where the vendor wiki is wrong. */

#include "driver/gpio.h"

/* Controls: a rocker (UP/DOWN/middle) + BOOT + PWR. All momentary,
 * active-low. There is NO rotary encoder on this board. */
#define BOARD_BTN_UP_GPIO        GPIO_NUM_4
#define BOARD_BTN_DOWN_GPIO      GPIO_NUM_6
#define BOARD_BTN_FN_GPIO        GPIO_NUM_5   /* rocker middle push */
#define BOARD_BTN_BOOT_GPIO      GPIO_NUM_0   /* ACTION / push-to-talk; strap pin */

/* PWR is not a GPIO: it is the AXP2101 power key. Short/long presses arrive
 * as PMIC interrupts (PKEY_SHORT_IRQ / PKEY_LONG_IRQ) on this line. */
#define BOARD_PMIC_IRQ_GPIO      GPIO_NUM_38

/* E-paper: SSD1677 on SPI3 (write-only). Framebuffer is 800*480/8 = 48000. */
#define BOARD_EPD_HOST           SPI3_HOST
#define BOARD_EPD_DC_GPIO        GPIO_NUM_9
#define BOARD_EPD_CS_GPIO        GPIO_NUM_10
#define BOARD_EPD_SCK_GPIO       GPIO_NUM_11
#define BOARD_EPD_MOSI_GPIO      GPIO_NUM_12
#define BOARD_EPD_RST_GPIO       GPIO_NUM_46  /* strap pin: care at reset */
#define BOARD_EPD_BUSY_GPIO      GPIO_NUM_3   /* strap pin; busy level = 1 */
#define BOARD_EPD_WIDTH          800
#define BOARD_EPD_HEIGHT         480
#define BOARD_EPD_BUFFER_LEN     ((BOARD_EPD_WIDTH * BOARD_EPD_HEIGHT) / 8)

/* Shared I2C bus: I2C_NUM_1, 400 kHz. */
#define BOARD_I2C_PORT           I2C_NUM_1
#define BOARD_I2C_SDA_GPIO       GPIO_NUM_41
#define BOARD_I2C_SCL_GPIO       GPIO_NUM_42
#define BOARD_I2C_FREQ_HZ        400000
#define BOARD_ES8311_ADDR        0x30
#define BOARD_AXP2101_ADDR       0x34
#define BOARD_PCF85063_ADDR      0x51
#define BOARD_QMI8658_ADDR       0x6B  /* fallback 0x6A */
#define BOARD_SHTC3_ADDR         0x70  /* no driver yet */

/* Interrupts */
#define BOARD_RTC_INT_GPIO       GPIO_NUM_45  /* PCF85063; strap pin */
#define BOARD_IMU_INT_GPIO       GPIO_NUM_40  /* QMI8658 INT2 */

/* Audio: ES8311 over I2S0, full-duplex, 16 kHz mono 16-bit.
 * NS4150B amp enable on GPIO39; run output at full scale. */
#define BOARD_I2S_MCLK_GPIO      GPIO_NUM_13
#define BOARD_I2S_BCLK_GPIO      GPIO_NUM_14
#define BOARD_I2S_WS_GPIO        GPIO_NUM_47
#define BOARD_I2S_DIN_GPIO       GPIO_NUM_21
#define BOARD_I2S_DOUT_GPIO      GPIO_NUM_48
#define BOARD_PA_GPIO            GPIO_NUM_39
#define BOARD_AUDIO_RATE_HZ      16000

/* MicroSD, SDMMC 4-bit (not used by v1; here for completeness). */
#define BOARD_SD_CLK_GPIO        GPIO_NUM_16
#define BOARD_SD_CMD_GPIO        GPIO_NUM_17
#define BOARD_SD_D0_GPIO         GPIO_NUM_15
#define BOARD_SD_D1_GPIO         GPIO_NUM_7
#define BOARD_SD_D2_GPIO         GPIO_NUM_8
#define BOARD_SD_D3_GPIO         GPIO_NUM_18
