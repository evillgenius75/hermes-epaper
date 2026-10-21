#include "epd.h"

#include <string.h>

#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* SSD1677 command opcodes (datasheet §"Command Table"). */
#define CMD_DRIVER_OUTPUT     0x01
#define CMD_BOOSTER_SOFT_START 0x0C
#define CMD_DEEP_SLEEP        0x10
#define CMD_DATA_ENTRY_MODE   0x11
#define CMD_SW_RESET          0x12
#define CMD_TEMP_SENSOR       0x18
#define CMD_MASTER_ACTIVATE   0x20
#define CMD_UPDATE_CONTROL_1  0x21
#define CMD_UPDATE_CONTROL_2  0x22
#define CMD_WRITE_RAM_BW      0x24
#define CMD_WRITE_RAM_OLD     0x26
#define CMD_WRITE_VCOM        0x2C
#define CMD_BORDER_WAVEFORM   0x3C
#define CMD_RAM_X_WINDOW      0x44
#define CMD_RAM_Y_WINDOW      0x45
#define CMD_RAM_X_COUNTER     0x4E
#define CMD_RAM_Y_COUNTER     0x4F

/* Display-update sequence stage bytes for CMD_UPDATE_CONTROL_2.
 * Bits select: clock on, analog on, load temperature, load LUT,
 * display, analog off, clock off. FF = full sequence with OTP LUT and
 * temperature load (initial/full update). C7 = pattern display only,
 * no temperature/LUT reload (partial update). BENCH: confirm against
 * the panel's OTP behavior at bring-up. */
#define UPDATE_SEQ_FULL    0xFF
#define UPDATE_SEQ_PARTIAL 0xC7

#define BUSY_TIMEOUT_MS 10000 /* hardware fact: reference driver value */
#define SPI_CLOCK_HZ (10 * 1000 * 1000)

static spi_device_handle_t s_spi;
static uint8_t *s_fb;      /* current frame */
static uint8_t *s_prev;    /* last displayed frame (old plane) */
static bool s_sleeping;
static bool s_ready;

/* ---- low-level transport ------------------------------------------------- */

static inline void cs(int level) { gpio_set_level(BOARD_EPD_CS_GPIO, level); }
static inline void dc(int level) { gpio_set_level(BOARD_EPD_DC_GPIO, level); }

static esp_err_t spi_write(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t send_cmd(uint8_t cmd)
{
    dc(0);
    cs(0);
    esp_err_t err = spi_write(&cmd, 1);
    cs(1);
    return err;
}

static esp_err_t send_data(const uint8_t *data, size_t len)
{
    dc(1);
    cs(0);
    esp_err_t err = spi_write(data, len);
    cs(1);
    return err;
}

static esp_err_t cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send_cmd(cmd);
    if (err == ESP_OK && len > 0) err = send_data(data, len);
    return err;
}

static esp_err_t wait_idle(void)
{
    /* BUSY is high while the panel works (busy level = 1, hardware fact). */
    int waited = 0;
    while (gpio_get_level(BOARD_EPD_BUSY_GPIO) == 1) {
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "panel busy timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    return ESP_OK;
}

static void hw_reset(void)
{
    /* Reset pulse timings are hardware facts from the reference project:
     * 2 ms low, 50 ms high/settle. */
    gpio_set_level(BOARD_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---- panel setup ------------------------------------------------------------ */

static esp_err_t set_window_full(void)
{
    const uint8_t x_win[2] = {0x00, (BOARD_EPD_WIDTH / 8) - 1};       /* 0..99 */
    const uint8_t y_win[4] = {0x00, 0x00,
                              (BOARD_EPD_HEIGHT - 1) & 0xFF,
                              ((BOARD_EPD_HEIGHT - 1) >> 8) & 0xFF};
    esp_err_t err = cmd_data(CMD_RAM_X_WINDOW, x_win, sizeof(x_win));
    if (err == ESP_OK) err = cmd_data(CMD_RAM_Y_WINDOW, y_win, sizeof(y_win));
    if (err == ESP_OK) err = cmd_data(CMD_RAM_X_COUNTER, (uint8_t[]){0x00}, 1);
    if (err == ESP_OK) err = cmd_data(CMD_RAM_Y_COUNTER, (uint8_t[]){0x00, 0x00}, 2);
    return err;
}

static esp_err_t panel_init_sequence(void)
{
    hw_reset();
    esp_err_t err = wait_idle();
    if (err != ESP_OK) return err;

    err = send_cmd(CMD_SW_RESET);
    if (err == ESP_OK) err = wait_idle();
    if (err != ESP_OK) return err;

    /* Gate driver output: MUX = height-1, scan from first gate. */
    const uint8_t drv[3] = {
        (BOARD_EPD_HEIGHT - 1) & 0xFF,
        ((BOARD_EPD_HEIGHT - 1) >> 8) & 0x01,
        0x00,
    };
    err = cmd_data(CMD_DRIVER_OUTPUT, drv, sizeof(drv));
    /* Data entry: +X, +Y, X-first address auto-increment. */
    if (err == ESP_OK) err = cmd_data(CMD_DATA_ENTRY_MODE, (uint8_t[]){0x03}, 1);
    /* Border follows the B/W RAM during full updates. */
    if (err == ESP_OK) err = cmd_data(CMD_BORDER_WAVEFORM, (uint8_t[]){0x01}, 1);
    /* Internal temperature sensor drives the OTP waveform selection. */
    if (err == ESP_OK) err = cmd_data(CMD_TEMP_SENSOR, (uint8_t[]){0x80}, 1);
    if (err == ESP_OK) err = set_window_full();
    s_sleeping = false;
    return err;
}

esp_err_t epd_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_EPD_CS_GPIO) | (1ULL << BOARD_EPD_DC_GPIO) |
                        (1ULL << BOARD_EPD_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_EPD_BUSY_GPIO),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    cs(1);
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_EPD_MOSI_GPIO,
        .miso_io_num = -1, /* write-only panel */
        .sclk_io_num = BOARD_EPD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_EPD_BUFFER_LEN,
    };
    esp_err_t err = spi_bus_initialize(BOARD_EPD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1, /* CS is driven manually (D/C framing) */
        .queue_size = 1,
    };
    err = spi_bus_add_device(BOARD_EPD_HOST, &dev, &s_spi);
    if (err != ESP_OK) return err;

    if (!s_fb) {
        s_fb = heap_caps_malloc(BOARD_EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM);
        s_prev = heap_caps_malloc(BOARD_EPD_BUFFER_LEN, MALLOC_CAP_SPIRAM);
        if (!s_fb || !s_prev) return ESP_ERR_NO_MEM;
        memset(s_fb, 0xFF, BOARD_EPD_BUFFER_LEN);
        memset(s_prev, 0xFF, BOARD_EPD_BUFFER_LEN);
    }

    err = panel_init_sequence();
    if (err == ESP_OK) s_ready = true;
    return err;
}

/* ---- framebuffer ----------------------------------------------------------- */

uint8_t *epd_framebuffer(void) { return s_fb; }

void epd_clear(bool white)
{
    memset(s_fb, white ? 0xFF : 0x00, BOARD_EPD_BUFFER_LEN);
}

void epd_draw_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= BOARD_EPD_WIDTH || y < 0 || y >= BOARD_EPD_HEIGHT) return;
    size_t idx = (size_t)y * (BOARD_EPD_WIDTH / 8) + (size_t)(x / 8);
    uint8_t mask = (uint8_t)(0x80 >> (x % 8));
    if (black) {
        s_fb[idx] &= (uint8_t)~mask;
    } else {
        s_fb[idx] |= mask;
    }
}

/* ---- refresh ----------------------------------------------------------------- */

static esp_err_t ensure_awake(void)
{
    if (!s_ready) return epd_init();
    if (s_sleeping) return panel_init_sequence(); /* hw reset wakes the panel */
    return ESP_OK;
}

static esp_err_t run_update(uint8_t sequence)
{
    esp_err_t err = cmd_data(CMD_UPDATE_CONTROL_2, &sequence, 1);
    if (err == ESP_OK) err = send_cmd(CMD_MASTER_ACTIVATE);
    if (err == ESP_OK) err = wait_idle();
    if (err == ESP_OK) memcpy(s_prev, s_fb, BOARD_EPD_BUFFER_LEN);
    return err;
}

static esp_err_t write_planes(bool with_old)
{
    esp_err_t err = set_window_full();
    if (err != ESP_OK) return err;
    if (with_old) {
        err = send_cmd(CMD_WRITE_RAM_OLD);
        if (err == ESP_OK) err = send_data(s_prev, BOARD_EPD_BUFFER_LEN);
        if (err != ESP_OK) return err;
        err = set_window_full();
        if (err != ESP_OK) return err;
    }
    err = send_cmd(CMD_WRITE_RAM_BW);
    if (err == ESP_OK) err = send_data(s_fb, BOARD_EPD_BUFFER_LEN);
    return err;
}

esp_err_t epd_refresh_full(void)
{
    esp_err_t err = ensure_awake();
    if (err == ESP_OK) err = write_planes(false);
    if (err == ESP_OK) err = run_update(UPDATE_SEQ_FULL);
    return err;
}

esp_err_t epd_refresh_fast(void)
{
    /* BENCH: panels whose OTP carries a fast waveform select it through
     * the same full sequence; if this panel's OTP lacks one, fast falls
     * back to the standard full update (correct, just not faster). */
    return epd_refresh_full();
}

esp_err_t epd_refresh_partial(void)
{
    esp_err_t err = ensure_awake();
    if (err == ESP_OK) err = write_planes(true);
    if (err == ESP_OK) err = run_update(UPDATE_SEQ_PARTIAL);
    return err;
}

esp_err_t epd_sleep(void)
{
    if (!s_ready || s_sleeping) return ESP_OK;
    esp_err_t err = cmd_data(CMD_DEEP_SLEEP, (uint8_t[]){0x01}, 1);
    if (err == ESP_OK) s_sleeping = true;
    return err;
}

bool epd_is_sleeping(void) { return s_sleeping; }
