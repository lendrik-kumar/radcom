/**
 * hardware_config.h — single source of truth for all hardware-specific values
 *
 * Edit this file to retarget the firmware to any ESP32 variant or radio module.
 * No other source file should contain hardcoded pin numbers, frequencies, or
 * network credentials.
 *
 * Sections:
 *   1. SPI bus — which ESP32 GPIOs connect to the radio module
 *   2. LoRa / SX1278 RF parameters — frequency, spreading factor, power, etc.
 *   3. Protocol timing — polling cycle timeouts, TX watchdog
 *   4. FreeRTOS queue depths — memory vs. burst-tolerance tradeoff
 *   5. WiFi AP — SSID, password, channel, station cap
 */

#pragma once

/* ══════════════════════════════════════════════════════════════════════════════
 * 1. SPI BUS — ESP32 GPIO → Ra-02 (SX1278) pin mapping
 *
 *  Current target: ESP32-C3 mini
 *
 *  Wiring:
 *    GPIO 6  → SCK   (SPI clock)
 *    GPIO 7  → MOSI  (master out, slave in)
 *    GPIO 2  → MISO  (master in, slave out)
 *    GPIO 10 → NSS   (chip select, active low)
 *    GPIO 3  → RESET (radio hardware reset)
 *    GPIO 1  → DIO0  (RxDone / TxDone interrupt)
 *
 *  Ra-02 power: 3.3V ONLY. Never connect to 5V.
 *  Always attach a 433 MHz antenna before TX.
 * ══════════════════════════════════════════════════════════════════════════════ */
#define HW_PIN_SCK    6
#define HW_PIN_MOSI   7
#define HW_PIN_MISO   2
#define HW_PIN_NSS   10   /* chip select */
#define HW_PIN_RST    3   /* radio reset — pulled low for 10 ms to reset */
#define HW_PIN_DIO0   1   /* DIO0: RxDone in RX mode, TxDone in TX mode */

/* Hardcoded Node ID for bench testing (1 to 254). 
 * Set to 0 to require NVS provisioning via HTTP. */
#define HW_NODE_ID    1

/* SPI clock speed. SX1278 max is 10 MHz; 8 MHz gives headroom on long wires. */
#define HW_SPI_CLK_HZ  8000000

/* ══════════════════════════════════════════════════════════════════════════════
 * 2. LoRa / SX1278 RF PARAMETERS
 *
 *  Current target: 433 MHz band (licensed, permitted)
 *  Radio module:   Ra-02 (AI-Thinker), SX1278, 18-pin U.FL
 *
 *  SX1278 frequency range: ~420 – 525 MHz
 *  Do NOT set HW_LORA_FREQ_HZ below 420 000 000 or above 525 000 000.
 *
 *  Link budget at current settings (1 km, minor obstacles):
 *    Free-space path loss @ 433 MHz, 1 km ≈ 85 dB
 *    + 15–20 dB obstruction margin                  ≈ 100–105 dB total loss
 *    Received power = 20 dBm − 103 dB              ≈ −83 dBm
 *    SF7 sensitivity                                ≈ −123 dBm
 *    Link margin                                    ≈ 40 dB  ✓
 * ══════════════════════════════════════════════════════════════════════════════ */

/* Centre frequency in Hz. */
#define HW_LORA_FREQ_HZ     433000000UL

/* Signal bandwidth in kHz. Standard LoRa values: 7.8 / 10.4 / 15.6 / 20.8 /
 * 31.25 / 41.7 / 62.5 / 125 / 250 / 500.
 * 125 kHz is the standard channel width used everywhere in this project. */
#define HW_LORA_BW_KHZ      125

/* Spreading factor: 6 – 12. Higher SF = longer range, lower throughput.
 * SF7 gives max throughput; the 40 dB link margin above proves it's sufficient.
 * Increase to SF8 only for nodes with heavy obstruction confirmed in field tests. */
#define HW_LORA_SF          7

/* Coding rate denominator offset: 1 = 4/5, 2 = 4/6, 3 = 4/7, 4 = 4/8.
 * 4/5 (value 1) is the most efficient. Increase only if you see frequent CRC errors. */
#define HW_LORA_CR          1

/* TX power in dBm. Ra-02 uses PA_BOOST pin; range 2–20 dBm.
 * 20 dBm is the module maximum. Don't exceed it — the Pa stage will clip. */
#define HW_LORA_TX_DBM      20

/* Preamble length in symbols. 8 is the LoRa default and works fine here. */
#define HW_LORA_PREAMBLE    8

/* Sync word — distinguishes this private network from other LoRa traffic.
 * 0x34 is the LoRaWAN public sync word — do NOT use it here.
 * 0x12 is the private-network convention. Change to any other non-0x34 byte
 * if you are co-located with other radcom deployments on the same frequency. */
#define HW_LORA_SYNC_WORD   0x12

/* ══════════════════════════════════════════════════════════════════════════════
 * 3. PROTOCOL TIMING
 *
 *  These values are derived from the polling protocol design (init.txt §5):
 *    - Each node's slot (POLL → DATA → ACK) takes roughly 150 ms at SF7.
 *    - Full cycle at 20 nodes = 20 × 150 ms = 3 000 ms.
 *    - We wait 2× the max cycle before declaring the master absent.
 * ══════════════════════════════════════════════════════════════════════════════ */

/* How long lora_task waits for a POLL before declaring master absent.
 * At 20 nodes × 150 ms/slot = 3 000 ms max cycle → 2× = 6 000 ms.
 * Reduce if you deploy fewer nodes and want faster "master offline" detection. */
#define HW_POLL_ABSENT_TIMEOUT_MS   6000

/* TX watchdog: maximum time we wait for TxDone IRQ after starting a TX.
 * If DIO0 doesn't fire within this window, we abort and re-enter RX.
 * At SF7, the longest packet (138 bytes) takes < 500 ms on air — 3 s is safe. */
#define HW_TX_WATCHDOG_MS           3000

/* WebSocket attach timeout: phone has this long after connecting to send
 * the {"type":"attach","phone":"..."} frame. Non-compliant clients are dropped. */
#define HW_ATTACH_TIMEOUT_MS        30000

/* ══════════════════════════════════════════════════════════════════════════════
 * 4. FREERTOS QUEUE DEPTHS
 *
 *  Each queue entry holds one radio_pkt_t (138 bytes) or one ws_event_t.
 *  Memory cost: depth × sizeof(item). Increase if you see "queue full" warnings
 *  under load; decrease to save RAM on memory-constrained variants.
 * ══════════════════════════════════════════════════════════════════════════════ */

/* lora_rx_queue: incoming packets from master → router_task.
 * LoRa is slow (one packet per ~150–500 ms), so 8 is generous. */
#define HW_LORA_RX_QUEUE_DEPTH    8

/* tx_lora_queue: outgoing packets from router_task → lora_task.
 * 16 = buffer for ~2 full poll cycles at 8 nodes. */
#define HW_LORA_TX_QUEUE_DEPTH   16

/* ws_event_queue: phone events (attach / detach / message) → router_task.
 * 32 handles bursts of phones reconnecting simultaneously. */
#define HW_WS_EVENT_QUEUE_DEPTH  32

/* ══════════════════════════════════════════════════════════════════════════════
 * 5. WiFi ACCESS POINT
 *
 *  All nodes use the same SSID so phones automatically roam to the strongest
 *  signal without user intervention (802.11 association handles it).
 *
 *  AP_MAX_STATIONS is capped at 10 by ESP32-C3 hardware, regardless of this
 *  value. The v2 upgrade path (init.txt §10) replaces the ESP32-C3's AP with
 *  a dedicated OpenWrt router; at that point this cap no longer applies.
 * ══════════════════════════════════════════════════════════════════════════════ */

/* WiFi network name. All nodes must share the same SSID for seamless roaming. */
#define HW_AP_SSID         "radcom"

/* WPA2 passphrase. Minimum 8 characters, or empty string for OPEN network. */
#define HW_AP_PASS         "gdgtiet1"

/* 2.4 GHz channel (1–13). Pick a channel not used by nearby infrastructure WiFi.
 * Channel 1 is used here to avoid common congestion on 6. */
#define HW_AP_CHANNEL      1

/* Maximum simultaneous WiFi stations. ESP32-C3 hardware limit is 10. */
#define HW_AP_MAX_STATIONS 10

/* Node IP address as string (used by DNS redirect and captive portal HTTP). */
#define HW_AP_IP           "192.168.4.1"

/* HTTP server port. 80 = standard; no reason to change for v1. */
#define HW_HTTP_PORT       80

/* UDP port for the DNS redirect server. 53 = standard DNS. */
#define HW_DNS_PORT        53
