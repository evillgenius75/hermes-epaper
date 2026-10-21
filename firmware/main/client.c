#include "client.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "client";

/* ---- config / identity -------------------------------------------------- */

static void device_id(char *out, size_t len)
{
    if (CONFIG_EPAPER_DEVICE_ID[0] != '\0') {
        strncpy(out, CONFIG_EPAPER_DEVICE_ID, len);
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void make_uuid(char *out, size_t len)
{
    uint32_t r[4];
    for (int i = 0; i < 4; i++) r[i] = esp_random();
    snprintf(out, len,
             "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",
             (unsigned long)r[0], (unsigned long)(r[1] & 0xffff),
             (unsigned long)(r[1] >> 16 & 0xfff),
             (unsigned long)(0x8000 | (r[2] & 0x3fff)),
             (unsigned long)r[3], (unsigned long)(r[2] >> 16 & 0xffff));
}

/* ---- HTTP plumbing -------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len, cap;
} resp_t;

static esp_err_t on_event(esp_http_client_event_t *e)
{
    resp_t *r = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && r && r->buf) {
        size_t need = r->len + e->data_len + 1;
        if (need > r->cap) {
            char *nb = realloc(r->buf, need * 2);
            if (!nb) return ESP_ERR_NO_MEM;
            r->buf = nb;
            r->cap = need * 2;
        }
        memcpy(r->buf + r->len, e->data, e->data_len);
        r->len += e->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

/* Performs a request; on success *json_out is a cJSON tree to delete. */
static esp_err_t request(const char *method, const char *path,
                         const char *body, const char *content_type,
                         const void *raw_body, size_t raw_len,
                         const char *hdr_name, const char *hdr_value,
                         cJSON **json_out)
{
    char url[320], id[32], auth[128];
    device_id(id, sizeof(id));
    snprintf(url, sizeof(url), "%s%s", CONFIG_EPAPER_SERVER_URL, path);
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_EPAPER_DEVICE_TOKEN);

    resp_t resp = { .buf = calloc(1, 1024), .cap = 1024 };
    if (!resp.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = on_event,
        .user_data = &resp,
        .timeout_ms = 55000, /* long-poll waits up to 50 s (protocol §4.3) */
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_header(h, "X-Device-Id", id);
    if (hdr_name) esp_http_client_set_header(h, hdr_name, hdr_value);
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(h, HTTP_METHOD_POST);
        if (raw_body) {
            esp_http_client_set_header(h, "Content-Type",
                                       content_type ? content_type : "application/octet-stream");
            esp_http_client_set_post_field(h, raw_body, raw_len);
        } else {
            esp_http_client_set_header(h, "Content-Type", "application/json");
            esp_http_client_set_post_field(h, body, body ? strlen(body) : 0);
        }
    }

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err == ESP_OK && status >= 200 && status < 300 && json_out) {
        *json_out = resp.buf ? cJSON_Parse(resp.buf) : NULL;
        if (!*json_out) err = ESP_ERR_INVALID_RESPONSE;
    } else if (err == ESP_OK && status >= 300) {
        ESP_LOGW(TAG, "%s %s -> HTTP %d", method, path, status);
        err = ESP_FAIL;
    }
    free(resp.buf);
    return err;
}

/* ---- protocol endpoints --------------------------------------------------- */

esp_err_t client_status(long *server_time_out, char *agent_state_out, size_t len)
{
    cJSON *root = NULL;
    esp_err_t err = request("GET", "/epaper/v1/status", NULL, NULL, NULL, 0, NULL, NULL, &root);
    if (err == ESP_OK && root) {
        cJSON *t = cJSON_GetObjectItem(root, "server_time");
        cJSON *s = cJSON_GetObjectItem(root, "agent_state");
        if (cJSON_IsNumber(t) && server_time_out) *server_time_out = (long)t->valuedouble;
        if (cJSON_IsString(s) && agent_state_out) strncpy(agent_state_out, s->valuestring, len);
        cJSON_Delete(root);
    }
    return err;
}

esp_err_t client_send_text(const char *text)
{
    char uuid[40];
    make_uuid(uuid, sizeof(uuid));
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "id", uuid);
    cJSON_AddStringToObject(body, "text", text);
    char *json = cJSON_PrintUnformatted(body);
    esp_err_t err = request("POST", "/epaper/v1/messages", json, NULL, NULL, 0, NULL, NULL, NULL);
    free(json);
    cJSON_Delete(body);
    return err;
}

esp_err_t client_send_voice(const void *wav, size_t len)
{
    char uuid[40];
    make_uuid(uuid, sizeof(uuid));
    return request("POST", "/epaper/v1/voice", NULL, "audio/wav", wav, len,
                   "X-Msg-Id", uuid, NULL);
}

esp_err_t client_poll_inbox(long after_seq, int wait_s,
                            inbox_item_t *items, int max, int *count_out)
{
    char path[96];
    snprintf(path, sizeof(path), "/epaper/v1/inbox?after=%ld&wait=%d", after_seq, wait_s);
    cJSON *root = NULL;
    esp_err_t err = request("GET", path, NULL, NULL, NULL, 0, NULL, NULL, &root);
    *count_out = 0;
    if (err != ESP_OK || !root) return err;

    cJSON *arr = cJSON_GetObjectItem(root, "items");
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (*count_out >= max) break;
        inbox_item_t *dst = &items[*count_out];
        memset(dst, 0, sizeof(*dst));
        cJSON *seq = cJSON_GetObjectItem(it, "seq");
        cJSON *kind = cJSON_GetObjectItem(it, "kind");
        cJSON *text = cJSON_GetObjectItem(it, "text");
        if (cJSON_IsNumber(seq)) dst->seq = (long)seq->valuedouble;
        dst->kind = (cJSON_IsString(kind) && strcmp(kind->valuestring, "prompt") == 0)
                        ? ITEM_PROMPT : ITEM_MESSAGE;
        if (cJSON_IsString(text)) dst->text = strdup(text->valuestring);
        if (dst->kind == ITEM_PROMPT) {
            cJSON *pid = cJSON_GetObjectItem(it, "prompt_id");
            cJSON *pt = cJSON_GetObjectItem(it, "prompt_type");
            cJSON *ch = cJSON_GetObjectItem(it, "choices");
            if (cJSON_IsString(pid)) strncpy(dst->id, pid->valuestring, sizeof(dst->id));
            if (cJSON_IsString(pt)) strncpy(dst->prompt_type, pt->valuestring, sizeof(dst->prompt_type));
            cJSON *c;
            cJSON_ArrayForEach(c, ch) {
                if (dst->choice_count < 8 && cJSON_IsString(c))
                    dst->choices[dst->choice_count++] = strdup(c->valuestring);
            }
        }
        (*count_out)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t client_ack(long upto_seq)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "upto", upto_seq);
    char *json = cJSON_PrintUnformatted(body);
    esp_err_t err = request("POST", "/epaper/v1/inbox/ack", json, NULL, NULL, 0, NULL, NULL, NULL);
    free(json);
    cJSON_Delete(body);
    if (err == ESP_OK) client_last_seq_store(upto_seq);
    return err;
}

esp_err_t client_resolve_prompt(const char *prompt_id, const char *choice)
{
    char path[128];
    snprintf(path, sizeof(path), "/epaper/v1/prompts/%s/resolve", prompt_id);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "choice", choice);
    char *json = cJSON_PrintUnformatted(body);
    esp_err_t err = request("POST", path, json, NULL, NULL, 0, NULL, NULL, NULL);
    free(json);
    cJSON_Delete(body);
    return err;
}

esp_err_t client_history(int limit, char *buf, size_t buf_len)
{
    (void)limit;
    /* The shared request() parses JSON; history rendering needs the raw
     * payload for now. TODO: parse into transcript entries in ui.c. */
    (void)buf; (void)buf_len;
    ESP_LOGW(TAG, "client_history: raw fetch TODO");
    return ESP_ERR_NOT_SUPPORTED;
}

void client_free_items(inbox_item_t *items, int count)
{
    for (int i = 0; i < count; i++) {
        free(items[i].text);
        for (int c = 0; c < items[i].choice_count; c++) free(items[i].choices[c]);
    }
}

/* ---- NVS last-acked seq ----------------------------------------------------- */

long client_last_seq_load(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("epaper", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "last_seq", &v);
        nvs_close(h);
    }
    return v;
}

void client_last_seq_store(long seq)
{
    nvs_handle_t h;
    if (nvs_open("epaper", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "last_seq", (int32_t)seq);
        nvs_commit(h);
        nvs_close(h);
    }
}
