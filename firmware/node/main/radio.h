/**
 * radio.h — SX1278 driver + lora_task public interface
 *
 * lora_task runs at the highest application priority (6).
 * It owns the SX1278 exclusively — no other code touches SPI.
 *
 * All pin numbers, RF parameters, and timing values come from hardware_config.h.
 * Do not add hardcoded values here — edit hardware_config.h instead.
 *
 * IPC contract:
 *   lora_rx_queue  (radio → router): packets received from master
 *   tx_lora_queue  (router → radio): packets to send on our next POLL slot
 * Both queues are allocated in main.c and passed to radio_init().
 */

#pragma once

#include "hardware_config.h"
#include "packet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"

/* ── Pin / RF / timing aliases ─────────────────────────────────────────────
 * Values live in hardware_config.h. These aliases keep radio.c readable
 * without it needing to know the HW_ prefix convention.
 * ─────────────────────────────────────────────────────────────────────────── */
#define RADIO_PIN_SCK           HW_PIN_SCK
#define RADIO_PIN_MOSI          HW_PIN_MOSI
#define RADIO_PIN_MISO          HW_PIN_MISO
#define RADIO_PIN_NSS           HW_PIN_NSS
#define RADIO_PIN_RST           HW_PIN_RST
#define RADIO_PIN_DIO0          HW_PIN_DIO0

#define LORA_FREQ_HZ            HW_LORA_FREQ_HZ
#define LORA_BW_KHZ             HW_LORA_BW_KHZ
#define LORA_SF                 HW_LORA_SF
#define LORA_CR                 HW_LORA_CR
#define LORA_TX_DBM             HW_LORA_TX_DBM
#define LORA_PREAMBLE           HW_LORA_PREAMBLE
#define LORA_SYNC_WORD          HW_LORA_SYNC_WORD

#define POLL_ABSENT_TIMEOUT_MS  HW_POLL_ABSENT_TIMEOUT_MS
#define TX_WATCHDOG_MS          HW_TX_WATCHDOG_MS

#define LORA_RX_QUEUE_DEPTH     HW_LORA_RX_QUEUE_DEPTH
#define LORA_TX_QUEUE_DEPTH     HW_LORA_TX_QUEUE_DEPTH

/* ── Shared state (written by lora_task, read by router_task) ───────────── */
extern TaskHandle_t      lora_task_handle;
extern volatile bool     master_absent;   /* true when no POLL within timeout */

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Initialise SPI bus, GPIO, and SX1278 registers.
 * Call once before lora_task is created. Returns ESP_OK or error code.
 */
esp_err_t radio_init(uint8_t node_id, QueueHandle_t rx_q, QueueHandle_t tx_q);

/**
 * FreeRTOS task entry point. Priority 6, stack 4096.
 * Owns SX1278 exclusively. Never call SX1278 functions from other tasks.
 */
void lora_task(void *arg);
