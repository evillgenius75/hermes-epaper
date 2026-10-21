#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* Device-protocol client — docs/protocol.md §4.
 * All calls are blocking; run them from the network task, never the UI task.
 * Delivery is at-least-once: message ids are generated here and retries
 * reuse the same id (the adapter deduplicates). */

typedef enum {
    ITEM_MESSAGE,
    ITEM_PROMPT,
} inbox_item_kind_t;

typedef struct {
    long seq;
    inbox_item_kind_t kind;
    char id[64];          /* message id / prompt_id */
    char prompt_type[16]; /* prompts: "approval" | "clarify" */
    char *text;           /* heap; free with client_free_items() */
    char *choices[8];     /* prompts only; heap */
    int choice_count;
    bool multi_select;
} inbox_item_t;

/* GET /status — also syncs the RTC from server_time. */
esp_err_t client_status(long *server_time_out, char *agent_state_out, size_t len);

/* POST /messages (202). `text` is UTF-8, NUL-terminated. */
esp_err_t client_send_text(const char *text);

/* POST /voice — WAV 16 kHz mono 16-bit from a memory buffer. */
esp_err_t client_send_voice(const void *wav, size_t len);

/* GET /inbox?after=&wait= — long-poll. Fills up to `max` items. */
esp_err_t client_poll_inbox(long after_seq, int wait_s,
                            inbox_item_t *items, int max, int *count_out);

/* POST /inbox/ack { upto } — call after rendering. */
esp_err_t client_ack(long upto_seq);

/* POST /prompts/<id>/resolve { choice }. */
esp_err_t client_resolve_prompt(const char *prompt_id, const char *choice);

/* GET /history?limit= — raw JSON into `buf` (render cache for boot). */
esp_err_t client_history(int limit, char *buf, size_t buf_len);

void client_free_items(inbox_item_t *items, int count);

/* Persistent last-acked seq (NVS). */
long client_last_seq_load(void);
void client_last_seq_store(long seq);
