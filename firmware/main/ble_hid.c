#include "ble_hid.h"

#include "sdkconfig.h"

#if CONFIG_EPAPER_BLE_KEYBOARD

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

/* NimBLE host */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

static const char *TAG = "ble_hid";

/* HID service / characteristics (Bluetooth SIG assigned numbers). */
static const ble_uuid16_t HID_SVC_UUID = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t REPORT_UUID = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(0x2902);

#define CONN_MIN_INTERVAL 24  /* 30 ms (1.25 ms units) */
#define CONN_MAX_INTERVAL 40  /* 50 ms */
#define CONN_LATENCY 4
#define CONN_TIMEOUT 200      /* 2 s (10 ms units) */

static QueueHandle_t s_out;
static bool s_stack_up;
static bool s_connected;
static bool s_pairing;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;
static ble_addr_t s_peer; /* bonded keyboard, persisted in NVS */

/* ---- event output ---------------------------------------------------------- */

static void post_char(char c)
{
    app_event_t ev = { .type = EV_TEXT };
    ev.text[0] = c;
    ev.text[1] = '\0';
    xQueueSend(s_out, &ev, 0);
}

static void post_event(app_event_type_t type)
{
    app_event_t ev = { .type = type };
    xQueueSend(s_out, &ev, 0);
}

/* US-layout keymap for HID usage IDs; index 0 = unshifted, 1 = shifted. */
static const char k_keymap[][2] = {
    [0x04] = {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'},
    {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'i', 'I'}, {'j', 'J'}, {'k', 'K'},
    {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'}, {'p', 'P'}, {'q', 'Q'},
    {'r', 'R'}, {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'},
    {'x', 'X'}, {'y', 'Y'}, {'z', 'Z'},
    [0x1E] = {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'},
    {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
    [0x2D] = {'-', '_'}, [0x2E] = {'=', '+'}, [0x2F] = {'[', '{'},
    [0x30] = {']', '}'}, [0x31] = {'\\', '|'}, [0x33] = {';', ':'},
    [0x34] = {'\'', '"'}, [0x35] = {'`', '~'}, [0x36] = {',', '<'},
    [0x37] = {'.', '>'}, [0x38] = {'/', '?'},
};

/* Parse one HID input report. Assumes the boot-keyboard report layout
 * (8 bytes: modifiers, reserved, 6 keycodes), which boot-protocol
 * keyboards send and most keyboards default to after pairing.
 * TODO: report-protocol keyboards prepend a report ID — read the Report
 * Reference descriptor (0x2908) and offset when present. */
static void handle_report(const uint8_t *data, size_t len)
{
    if (len < 8) return;
    bool shift = data[0] & 0x22; /* L/R shift bits */
    static uint8_t prev[6];
    for (int i = 0; i < 6; i++) {
        uint8_t code = data[2 + i];
        if (code == 0) continue;
        bool already = false;
        for (int j = 0; j < 6; j++) already |= (prev[j] == code);
        if (already) continue; /* still held; ignore repeats (OSK parity) */
        switch (code) {
        case 0x28: post_char('\n'); break;  /* Enter */
        case 0x29: post_event(EV_BACK); break; /* Esc */
        case 0x2A: post_char('\b'); break;  /* Backspace */
        case 0x2B: post_char('\t'); break;  /* Tab */
        case 0x2C: post_char(' '); break;   /* Space */
        default:
            if (code < sizeof(k_keymap) / sizeof(k_keymap[0]) &&
                k_keymap[code][shift ? 1 : 0]) {
                post_char(k_keymap[code][shift ? 1 : 0]);
            }
            break;
        }
    }
    memcpy(prev, data + 2, 6);
}

/* ---- GATT discovery ---------------------------------------------------------- */

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg);
static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg);
static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg);

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status != 0 || svc == NULL) {
        ESP_LOGW(TAG, "HID service not found");
        return 0;
    }
    ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle,
                             on_disc_chr, NULL);
    return 0;
}

static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status != 0 || chr == NULL) return 0; /* NULL = discovery done */
    if (ble_uuid_cmp(&chr->uuid.u, &REPORT_UUID.u) == 0 &&
        (chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
        s_report_handle = chr->val_handle;
        ble_gattc_disc_all_dscs(conn, chr->val_handle,
                                chr->val_handle + 4, on_disc_dsc, NULL);
    }
    return 0;
}

static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (err->status != 0 || dsc == NULL) return 0;
    if (ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0) {
        uint16_t value = 0x0001; /* enable notifications */
        ble_gattc_write_flat(conn, dsc->handle, &value, sizeof(value),
                             NULL, NULL);
        ESP_LOGI(TAG, "subscribed to keyboard reports");
    }
    return 0;
}

/* ---- GAP events ---------------------------------------------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 0x0030,
        .window = 0x0030,
        .passive = 0,
    };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 10000 /* ms */, &params,
                 gap_event_cb, NULL);
}

static void connect_peer(const ble_addr_t *addr)
{
    struct ble_gap_conn_params params = {
        .scan_itvl = 0x0030,
        .scan_window = 0x0030,
        .itvl_min = CONN_MIN_INTERVAL,
        .itvl_max = CONN_MAX_INTERVAL,
        .latency = CONN_LATENCY,
        .supervision_timeout = CONN_TIMEOUT,
    };
    ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, 30000, &params,
                    gap_event_cb, NULL);
}

static void save_bond(const ble_addr_t *addr)
{
    nvs_handle_t h;
    if (nvs_open("epaper", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "ble_peer", addr, sizeof(*addr));
        nvs_commit(h);
        nvs_close(h);
    }
    s_peer = *addr;
}

static bool load_bond(ble_addr_t *addr)
{
    nvs_handle_t h;
    size_t len = sizeof(*addr);
    bool ok = false;
    if (nvs_open("epaper", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "ble_peer", addr, &len) == ESP_OK &&
             len == sizeof(*addr);
        nvs_close(h);
    }
    return ok;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_pairing) {
            /* Look for the HID service UUID in the advertisement. */
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                        event->disc.length_data) == 0) {
                for (int i = 0; i < fields.num_uuids16; i++) {
                    if (fields.uuids16[i].value == 0x1812) {
                        ble_gap_disc_cancel();
                        connect_peer(&event->disc.addr);
                        break;
                    }
                }
            }
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            s_connected = false;
            if (s_pairing) start_scan();
            return 0;
        }
        s_connected = true;
        s_conn_handle = event->connect.conn_handle;
        if (s_pairing) {
            save_bond(&event->connect.peer_id_addr);
            s_pairing = false;
        }
        ble_gattc_disc_svc_by_uuid(s_conn_handle, &HID_SVC_UUID.u,
                                   on_disc_svc, NULL);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Reconnect quietly if we still have a bond; no scanning. */
        if (ble_hid_bonded()) connect_peer(&s_peer);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t buf[32];
        if (event->notify_rx.attr_handle == s_report_handle &&
            len <= sizeof(buf)) {
            os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
            handle_report(buf, len);
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* ---- stack lifecycle ------------------------------------------------------------- */

extern void ble_store_config_init(void);

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    if (ble_hid_bonded()) {
        connect_peer(&s_peer); /* bonded: straight to connect, no scan */
    }
}

static bool stack_start(void)
{
    if (s_stack_up) return true;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT; /* Just Works */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    s_stack_up = true;
    return true;
}

static void stack_stop(void)
{
    if (!s_stack_up) return;
    nimble_port_stop();
    nimble_port_deinit();
    s_stack_up = false;
    ESP_LOGI(TAG, "BLE stack deinitialized (zero idle cost)");
}

/* ---- public API --------------------------------------------------------------------- */

void ble_hid_boot(QueueHandle_t out)
{
    s_out = out;
    if (load_bond(&s_peer)) {
        ESP_LOGI(TAG, "bonded keyboard found; starting BLE");
        stack_start();
    }
    /* No bond: BLE stays completely off. */
}

bool ble_hid_start_pairing(void)
{
    s_pairing = true;
    if (!stack_start()) return false;
    /* If the stack just came up, on_sync fires first; scan from there in
     * that case. When already up, scan immediately. */
    if (!ble_hid_bonded()) start_scan();
    return true;
}

void ble_hid_stop_pairing(void)
{
    s_pairing = false;
    ble_gap_disc_cancel();
    if (!s_connected && !ble_hid_bonded()) {
        stack_stop(); /* back to zero cost */
    }
}

bool ble_hid_connected(void) { return s_connected; }

bool ble_hid_bonded(void)
{
    ble_addr_t addr;
    return s_peer.type != 0 || load_bond(&addr);
}

void ble_hid_forget(void)
{
    if (s_connected) ble_gap_terminate(s_conn_handle, 0x13);
    nvs_handle_t h;
    if (nvs_open("epaper", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ble_peer");
        nvs_commit(h);
        nvs_close(h);
    }
    memset(&s_peer, 0, sizeof(s_peer));
    /* TODO: also delete the stored LTK via ble_store_delete_nvs_class /
     * peer sec records so the keyboard forgets us too. */
    stack_stop();
}

#else /* CONFIG_EPAPER_BLE_KEYBOARD */

void ble_hid_boot(QueueHandle_t out) { (void)out; }
bool ble_hid_start_pairing(void) { return false; }
void ble_hid_stop_pairing(void) {}
bool ble_hid_connected(void) { return false; }
bool ble_hid_bonded(void) { return false; }
void ble_hid_forget(void) {}

#endif
