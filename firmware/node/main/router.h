/**
 * router.h — session table + message routing (router_task)
 *
 * router_task runs at priority 5.
 * Owns the session table exclusively — no locking needed because it's the
 * only writer. All inputs arrive via queues.
 *
 * IPC contract (inputs):
 *   lora_rx_queue  — packets from master (DATA deliveries, ACKs)
 *   ws_event_queue — phone attach/detach/message events
 *
 * IPC contract (outputs):
 *   tx_lora_queue  — packets to send in next POLL slot
 *   ws_send_json() — deliver messages to local phones
 */

#pragma once

#include "packet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

/* ── Session table ───────────────────────────────────────────────────────── */
#define MAX_SESSIONS  AP_MAX_STATIONS   /* 10 — matches WiFi AP hardware cap */

typedef struct {
    char  phone_num[UID_BUF_SIZE];
    int   ws_fd;
    bool  active;
} session_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise router state. Call before router_task is created.
 *
 * @param node_id   This node's ID (from NVS).
 * @param rx_q      lora_rx_queue (radio → router)
 * @param tx_q      tx_lora_queue (router → radio)
 * @param ws_q      ws_event_queue (wifi → router)
 */
esp_err_t router_init(uint8_t node_id,
                      QueueHandle_t rx_q,
                      QueueHandle_t tx_q,
                      QueueHandle_t ws_q);

/** FreeRTOS task entry. Priority 5, stack 4096. */
void router_task(void *arg);

/* ── Session table queries (safe to call from router_task only) ─────────── */

/** Find session by phone number. Returns pointer into sessions[], or NULL. */
session_t *session_find_by_phone(const char *phone_num);

/** Find session by WebSocket fd. Returns pointer into sessions[], or NULL. */
session_t *session_find_by_fd(int ws_fd);

/** Number of currently active sessions. */
int session_count(void);
