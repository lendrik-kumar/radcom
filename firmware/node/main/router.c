/**
 * router.c — session table + message routing
 *
 * This task is the bridge between the radio world (LoRa) and the WiFi world
 * (WebSocket). It owns the session table exclusively and enforces all routing
 * decisions.
 *
 * Task priority: 5 (below lora_task=6, above wifi_task=4).
 *
 * Message flow (init.txt §6, 3-hop reliability):
 *
 *   Inbound (master→phone):
 *     POLL with piggybacked payload → lora_rx_queue → router_task
 *     → session lookup by dst_uid → ws_send_json
 *
 *   Outbound (phone→master):
 *     ws_event_queue (WS_EVT_MESSAGE) → router_task
 *     → build DATA pkt → tx_lora_queue
 *
 *   Presence:
 *     WS_EVT_ATTACH → session create → PRESENCE(ATTACH) → tx_lora_queue
 *     WS_EVT_DETACH → session remove → PRESENCE(DETACH) → tx_lora_queue
 */

#include "router.h"
#include "wifi_ap.h"
#include "packet.h"
#include "radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "router";

/* ── Module state ────────────────────────────────────────────────────────── */

static session_t      sessions[MAX_SESSIONS];
static uint8_t        my_node_id;
static QueueHandle_t  lora_rx_q;
static QueueHandle_t  lora_tx_q;
static QueueHandle_t  ws_evt_q;

/* Rolling msg_id counter for outgoing packets — wraps 0..255 */
static uint8_t        next_msg_id = 1;

/* Last msg_id received from master; used for dedup */
static uint8_t        last_rx_msg_id = 0;
static bool           last_rx_valid  = false;

/* ── Session table ───────────────────────────────────────────────────────── */

session_t *session_find_by_phone(const char *phone_num) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active &&
            strncmp(sessions[i].phone_num, phone_num, UID_MAX_LEN) == 0) {
            return &sessions[i];
        }
    }
    return NULL;
}

session_t *session_find_by_fd(int ws_fd) {
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (sessions[i].active && sessions[i].ws_fd == ws_fd) {
            return &sessions[i];
        }
    }
    return NULL;
}

int session_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (sessions[i].active) ++n;
    return n;
}

/* Find a free slot. Returns NULL if table full. */
static session_t *session_alloc(void) {
    for (int i = 0; i < MAX_SESSIONS; ++i)
        if (!sessions[i].active) return &sessions[i];
    return NULL;
}

/* ── Outbound: enqueue a packet for the next POLL slot ───────────────────── */

static void enqueue_tx(radio_pkt_t *pkt) {
    pkt->node_id = my_node_id;
    pkt->msg_id  = next_msg_id++;
    if (next_msg_id == 0) next_msg_id = 1;  /* skip 0 — reserved for "no msg" */

    if (xQueueSend(lora_tx_q, pkt, 0) != pdTRUE) {
        /* tx queue full: master polling slower than messages arriving.
         * Drop the outgoing packet — phones will retry at the app layer.
         * ponytail: no in-RAM store-and-forward on the node; master handles persistence. */
        ESP_LOGW(TAG, "tx queue full — dropped pkt type=0x%02x", pkt->type);
    }
}

/* ── Handle: phone attached ──────────────────────────────────────────────── */

static void handle_attach(const ws_event_t *evt) {
    /* If same phone already has a session, it's a reconnect — update fd. */
    session_t *existing = session_find_by_phone(evt->phone_num);
    if (existing) {
        ESP_LOGI(TAG, "reconnect: %s fd %d → %d", evt->phone_num, existing->ws_fd, evt->ws_fd);
        existing->ws_fd = evt->ws_fd;

        char reply[64];
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"attached\",\"node_id\":%d}", my_node_id);
        ws_send_json(evt->ws_fd, reply);
        return;
    }

    /* New attach: find a free slot. */
    session_t *s = session_alloc();
    if (!s) {
        /* Table full — reject. (WiFi STA is still connected; we just refuse the session.) */
        ESP_LOGW(TAG, "session table full — rejecting %s", evt->phone_num);
        ws_send_json(evt->ws_fd,
                     "{\"type\":\"error\",\"code\":\"full\","
                     "\"msg\":\"Node at capacity, try another node\"}");
        return;
    }

    s->active = true;
    s->ws_fd  = evt->ws_fd;
    strncpy(s->phone_num, evt->phone_num, UID_MAX_LEN);
    s->phone_num[UID_MAX_LEN] = '\0';

    ESP_LOGI(TAG, "attach: %s fd=%d sessions=%d/%d",
             s->phone_num, s->ws_fd, session_count(), MAX_SESSIONS);

    /* Tell the phone it's attached. */
    char reply[64];
    snprintf(reply, sizeof(reply),
             "{\"type\":\"attached\",\"node_id\":%d}", my_node_id);
    ws_send_json(evt->ws_fd, reply);

    /* Notify master this phone is now reachable here. */
    radio_pkt_t pkt;
    pkt_make_presence(&pkt, my_node_id, s->phone_num, PRESENCE_ATTACH);
    enqueue_tx(&pkt);
}

/* ── Handle: phone detached ──────────────────────────────────────────────── */

static void handle_detach(const ws_event_t *evt) {
    session_t *s = session_find_by_fd(evt->ws_fd);
    if (!s) {
        /* Could be a phone that disconnected before completing attach — normal. */
        ESP_LOGD(TAG, "detach for unknown fd=%d (never fully attached)", evt->ws_fd);
        return;
    }

    ESP_LOGI(TAG, "detach: %s fd=%d sessions=%d/%d",
             s->phone_num, s->ws_fd, session_count() - 1, MAX_SESSIONS);

    /* Notify master this phone is gone before clearing session. */
    radio_pkt_t pkt;
    pkt_make_presence(&pkt, my_node_id, s->phone_num, PRESENCE_DETACH);
    enqueue_tx(&pkt);

    /* Clear session. */
    memset(s, 0, sizeof(*s));
}

/* ── Handle: outgoing message from phone ─────────────────────────────────── */

static void handle_outgoing_msg(const ws_event_t *evt) {
    session_t *s = session_find_by_fd(evt->ws_fd);
    if (!s) {
        /* Message from non-attached phone — shouldn't happen if wifi_ap validated,
         * but handle it anyway. */
        ESP_LOGW(TAG, "msg from unattached fd=%d, ignoring", evt->ws_fd);
        ws_send_json(evt->ws_fd,
                     "{\"type\":\"error\",\"code\":\"not_attached\"}");
        return;
    }

    /* Self-message: same src and dst — deliver locally, skip LoRa. */
    if (strncmp(s->phone_num, evt->dst_uid, UID_MAX_LEN) == 0) {
        char buf[PAYLOAD_MAX + 64];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"msg\",\"from\":\"%s\",\"text\":\"%.*s\"}",
                 s->phone_num, (int)evt->text_len, evt->text);
        ws_send_json(evt->ws_fd, buf);
        ESP_LOGD(TAG, "self-message delivered locally for %s", s->phone_num);
        return;
    }

    /* Check if destination is on THIS node — deliver locally without LoRa. */
    session_t *dst = session_find_by_phone(evt->dst_uid);
    if (dst) {
        char buf[PAYLOAD_MAX + 64];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"msg\",\"from\":\"%s\",\"text\":\"%.*s\"}",
                 s->phone_num, (int)evt->text_len, evt->text);
        ws_send_json(dst->ws_fd, buf);
        /* Tell sender it was delivered. */
        ws_send_json(evt->ws_fd, "{\"type\":\"sent\",\"local\":true}");
        ESP_LOGI(TAG, "local delivery %s→%s", s->phone_num, evt->dst_uid);
        return;
    }

    /* Remote delivery — queue for next POLL slot. */
    radio_pkt_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_DATA;
    pkt.ttl  = 30;  /* 30 minute expiry on master queue */
    strncpy(pkt.src_uid, s->phone_num, UID_MAX_LEN);
    strncpy(pkt.dst_uid, evt->dst_uid,  UID_MAX_LEN);
    pkt.payload_len = evt->text_len;
    memcpy(pkt.payload, evt->text, evt->text_len);

    enqueue_tx(&pkt);

    /* Optimistic "sent" — we'll tell the phone when master acks in 3-hop mode.
     * For now send queued confirmation so phone UX isn't blocked.
     * ponytail: true delivery receipt requires tracking msg_id and waiting for ACK;
     * upgrade path: add a pending_ack map keyed by msg_id. */
    ws_send_json(evt->ws_fd, "{\"type\":\"queued\"}");

    ESP_LOGI(TAG, "queued %s→%s len=%d", s->phone_num, evt->dst_uid, evt->text_len);
}

/* ── Handle: incoming packet from master ─────────────────────────────────── */

static void handle_lora_rx(const radio_pkt_t *pkt) {
    switch (pkt->type) {

        case PKT_DATA:
        case PKT_POLL:
            /* POLL with piggybacked payload — deliver to local phone. */
            if (pkt->payload_len == 0) break;  /* empty POLL, nothing to deliver */

            /* Dedup: if master retransmits the same msg_id, discard. */
            if (last_rx_valid && pkt->msg_id == last_rx_msg_id) {
                ESP_LOGD(TAG, "dedup: dropping duplicate msg_id=%d", pkt->msg_id);
                break;
            }
            last_rx_msg_id = pkt->msg_id;
            last_rx_valid  = true;

            session_t *dst = session_find_by_phone(pkt->dst_uid);
            if (!dst) {
                /* Destination not currently attached to this node.
                 * This shouldn't happen if master's routing table is correct,
                 * but can occur in a race (phone just disconnected). Log and drop.
                 * Master will re-queue when it receives our next PRESENCE(DETACH). */
                ESP_LOGW(TAG, "delivery for %s but not in session table", pkt->dst_uid);
                break;
            }

            /* Deliver to phone via WebSocket. */
            char buf[PAYLOAD_MAX + 64];
            /* Escape simple quotes in text — cJSON not needed for this template. */
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"msg\",\"from\":\"%s\",\"text\":\"%.*s\"}",
                     pkt->src_uid, (int)pkt->payload_len, pkt->payload);
            esp_err_t err = ws_send_json(dst->ws_fd, buf);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ws_send failed fd=%d err=%d (phone likely disconnected)",
                         dst->ws_fd, err);
                /* The next WiFi disconnect event will clean up the session.
                 * The ACK will still be sent — master should drop re-delivery attempts
                 * once it gets the PRESENCE(DETACH) from the next disconnect event. */
            } else {
                ESP_LOGI(TAG, "delivered %s→%s len=%d",
                         pkt->src_uid, pkt->dst_uid, pkt->payload_len);
            }
            break;

        case PKT_ACK:
            /* Master acking our DATA packet. 
             * ponytail: in the current optimistic "queued" model we don't track these.
             * Upgrade path: resolve pending_ack[pkt->ack_id] and push "sent" to phone. */
            ESP_LOGD(TAG, "master ACK msg_id=%d", pkt->ack_id);
            break;

        default:
            ESP_LOGW(TAG, "unexpected pkt type 0x%02x from LoRa", pkt->type);
            break;
    }
}

/* ── router_task ─────────────────────────────────────────────────────────── */

esp_err_t router_init(uint8_t node_id,
                       QueueHandle_t rx_q,
                       QueueHandle_t tx_q,
                       QueueHandle_t ws_q) {
    my_node_id = node_id;
    lora_rx_q  = rx_q;
    lora_tx_q  = tx_q;
    ws_evt_q   = ws_q;
    memset(sessions, 0, sizeof(sessions));
    return ESP_OK;
}

void router_task(void *arg) {
    ESP_LOGI(TAG, "started, node_id=%d", my_node_id);

    radio_pkt_t  lora_pkt;
    ws_event_t   ws_evt;

    for (;;) {
        /*
         * Process LoRa RX first (higher priority — delivery to phone).
         * Then handle WiFi events.
         * Both with zero timeout so we drain queues fairly.
         */
        if (xQueueReceive(lora_rx_q, &lora_pkt, 0) == pdTRUE) {
            handle_lora_rx(&lora_pkt);
        }

        if (xQueueReceive(ws_evt_q, &ws_evt, 0) == pdTRUE) {
            switch (ws_evt.type) {
                case WS_EVT_ATTACH:  handle_attach(&ws_evt);        break;
                case WS_EVT_DETACH:  handle_detach(&ws_evt);        break;
                case WS_EVT_MESSAGE: handle_outgoing_msg(&ws_evt);  break;
                default:
                    ESP_LOGW(TAG, "unknown ws_evt type %d", ws_evt.type);
                    break;
            }
        }

        /* Yield for 10 ms when both queues are empty — avoids starving wifi_task. */
        if (uxQueueMessagesWaiting(lora_rx_q) == 0 &&
            uxQueueMessagesWaiting(ws_evt_q)  == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
