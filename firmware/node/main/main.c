/**
 * main.c — radcom node entry point
 *
 * Responsibilities:
 *   1. Read node_id from NVS (or serve config portal if not provisioned)
 *   2. Create FreeRTOS queues
 *   3. Init radio, wifi_ap, router in dependency order
 *   4. Create all tasks with correct priorities and stack sizes
 *
 * Task priority map (higher = more urgent):
 *   lora_task   : 6  — must preempt everything to catch POLL window
 *   router_task : 5  — message routing
 *   wifi_task   : 4  — network I/O
 *   (idle)      : 0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "packet.h"
#include "radio.h"
#include "wifi_ap.h"
#include "router.h"

static const char *TAG = "main";

/* ── NVS namespace ───────────────────────────────────────────────────────── */
#define NVS_NAMESPACE  "radcom"
#define NVS_KEY_NODEID "node_id"

/* ── Queue handles (global — shared across modules via extern in headers) ─── */
QueueHandle_t lora_rx_queue;
QueueHandle_t tx_lora_queue;
QueueHandle_t ws_event_queue;

/* ── Read node_id from NVS ───────────────────────────────────────────────── */

static esp_err_t nvs_read_node_id(uint8_t *node_id) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint8_t id = 0;
    err = nvs_get_u8(h, NVS_KEY_NODEID, &id);
    nvs_close(h);

    if (err == ESP_OK && id > 0 && id < 255) {
        *node_id = id;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ── Write node_id to NVS (called from provisioning HTTP endpoint) ────────── */
esp_err_t nvs_write_node_id(uint8_t node_id) {
    if (node_id == 0 || node_id == 255) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_NODEID, node_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "radcom node firmware starting");

    /* NVS must be initialised before any NVS read (including WiFi internally). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition truncated or format mismatch — erase and reinitialise.
         * This wipes node_id; operator must re-provision. */
        ESP_LOGW(TAG, "NVS corrupt — erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(err == ESP_ERR_NVS_NO_FREE_PAGES ? ESP_OK : err);

    /* Read node_id. If not provisioned, use 0 as sentinel — radio_init will
     * skip LoRa operations and wifi_ap will serve a provisioning page. */
    uint8_t node_id = HW_NODE_ID;
    if (node_id == 0) {
        if (nvs_read_node_id(&node_id) != ESP_OK) {
            ESP_LOGW(TAG, "node_id not provisioned — serving config portal only");
            ESP_LOGW(TAG, "POST to http://192.168.4.1/config with {\"node_id\":N} to provision");
        } else {
            ESP_LOGI(TAG, "node_id=%d (from NVS)", node_id);
        }
    } else {
        ESP_LOGI(TAG, "node_id=%d (hardcoded in hardware_config.h)", node_id);
    }

    /* ── Create queues ────────────────────────────────────────────────────── */
    lora_rx_queue = xQueueCreate(LORA_RX_QUEUE_DEPTH, sizeof(radio_pkt_t));
    tx_lora_queue = xQueueCreate(LORA_TX_QUEUE_DEPTH, sizeof(radio_pkt_t));
    ws_event_queue = xQueueCreate(WS_EVENT_QUEUE_DEPTH, sizeof(ws_event_t));

    ESP_ERROR_CHECK(lora_rx_queue  ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(tx_lora_queue  ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(ws_event_queue ? ESP_OK : ESP_ERR_NO_MEM);

    /* ── Init modules (order matters: radio first, then wifi, then router) ── */
    if (node_id > 0) {
        err = radio_init(node_id, lora_rx_queue, tx_lora_queue);
        if (err != ESP_OK) {
            /* Radio hardware failure — node runs as WiFi-only relay (degraded mode).
             * Phones can still connect and get "no network" status. */
            ESP_LOGE(TAG, "radio_init failed (err=%d) — running WiFi-only (degraded)", err);
            node_id = 0;  /* treat as unprovisioned for radio purposes */
        }
    }

    ESP_ERROR_CHECK(wifi_ap_init(ws_event_queue));
    ESP_ERROR_CHECK(router_init(node_id, lora_rx_queue, tx_lora_queue, ws_event_queue));

    /* ── Create tasks ─────────────────────────────────────────────────────── */
    if (node_id > 0) {
        /* lora_task only starts when radio is healthy and node is provisioned. */
        BaseType_t rc = xTaskCreate(lora_task, "lora", 4096, NULL, 6, &lora_task_handle);
        ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }

    BaseType_t rc;
    rc = xTaskCreate(router_task, "router", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    rc = xTaskCreate(wifi_task, "wifi",   4096, NULL, 4, NULL);
    ESP_ERROR_CHECK(rc == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "all tasks started");
    /* app_main returns here — FreeRTOS scheduler takes over. */
}
