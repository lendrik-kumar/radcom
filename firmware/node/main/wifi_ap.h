/**
 * wifi_ap.h — WiFi AP + WebSocket server + captive portal + DNS redirect
 *
 * wifi_task runs at priority 4.
 * It owns all network I/O: AP mode, WebSocket server, HTTP server, DNS.
 *
 * All network credentials, channel, and port numbers come from hardware_config.h.
 * Do not add hardcoded values here — edit hardware_config.h instead.
 *
 * IPC contract:
 *   ws_event_queue (wifi → router): new messages/attach/detach events from phones
 *   ws_server      (global): httpd handle, used by router_task for async sends
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_http_server.h"
#include "esp_err.h"
#include "hardware_config.h"
#include "packet.h"

/* ── Network config aliases (values from hardware_config.h) ────────────────
 * wifi_ap.c uses these names directly; they map 1:1 to HW_ constants.
 * ────────────────────────────────────────────────────────────────────────── */
#define AP_SSID          HW_AP_SSID
#define AP_PASS          HW_AP_PASS
#define AP_CHANNEL       HW_AP_CHANNEL
#define AP_MAX_STATIONS  HW_AP_MAX_STATIONS
#define AP_IP            HW_AP_IP
#define WS_PATH          "/ws"            /* WebSocket endpoint — not hardware, keep here */
#define HTTP_PORT        HW_HTTP_PORT
#define DNS_PORT         HW_DNS_PORT

#define ATTACH_TIMEOUT_MS   HW_ATTACH_TIMEOUT_MS
#define WS_EVENT_QUEUE_DEPTH HW_WS_EVENT_QUEUE_DEPTH

/* ── WebSocket event types (carried in ws_event_queue) ──────────────────── */
typedef enum {
    WS_EVT_ATTACH,    /* new phone completed handshake: phone_num populated */
    WS_EVT_DETACH,    /* phone disconnected: phone_num populated, fd closed */
    WS_EVT_MESSAGE,   /* outgoing message from phone: all fields populated */
} ws_evt_type_t;

typedef struct {
    ws_evt_type_t type;
    int           ws_fd;            /* WebSocket file descriptor */
    char          phone_num[UID_BUF_SIZE];
    char          dst_uid[UID_BUF_SIZE];   /* MESSAGE only */
    uint8_t       text[PAYLOAD_MAX];
    uint8_t       text_len;
} ws_event_t;
/* ── Shared httpd handle (written by wifi_ap, read by router for async TX) ─ */
extern httpd_handle_t ws_server;

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise WiFi AP, HTTP server, WebSocket endpoint, and DNS redirect.
 * Call once from app_main before wifi_task is created.
 */
esp_err_t wifi_ap_init(QueueHandle_t evt_q);

/**
 * Send a JSON message to a specific WebSocket client (from any task).
 * Thread-safe — uses httpd_ws_send_frame_async internally.
 * Returns ESP_OK or error.
 */
esp_err_t ws_send_json(int ws_fd, const char *json_str);

/**
 * FreeRTOS task entry. Priority 4, stack 4096.
 * Runs the DNS server loop (HTTP/WS are event-driven via httpd).
 */
void wifi_task(void *arg);
