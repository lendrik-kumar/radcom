#include "radio.h"
#include "packet.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

/* ── Registers ───────────────────────────────────────────────────────────── */
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_PA_RAMP              0x0A
#define REG_OCP                  0x0B
#define REG_LNA                  0x0C
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS_MASK       0x11
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_RX_HEADER_CNT_VALUE_MSB 0x14
#define REG_RX_HEADER_CNT_VALUE_LSB 0x15
#define REG_RX_PACKET_CNT_VALUE_MSB 0x16
#define REG_RX_PACKET_CNT_VALUE_LSB 0x17
#define REG_MODEM_STAT           0x18
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_RSSI_VALUE           0x1B
#define REG_HOP_CHANNEL          0x1C
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_SYMB_TIMEOUT_LSB     0x1F
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MAX_PAYLOAD_LENGTH   0x23
#define REG_HOP_PERIOD           0x24
#define REG_FIFO_RX_BYTE_ADDR    0x25
#define REG_MODEM_CONFIG_3       0x26
#define REG_PPM_CORRECTION       0x27
#define REG_FEI_MSB              0x28
#define REG_FEI_MID              0x29
#define REG_FEI_LSB              0x2A
#define REG_RSSI_WIDEBAND        0x2C
#define REG_DETECT_OPTIMIZE      0x31
#define REG_INVERT_IQ            0x33
#define REG_DETECTION_THRESHOLD  0x37
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_DIO_MAPPING_2        0x41
#define REG_VERSION              0x42
#define REG_PA_DAC               0x4D

/* IRQ Flags */
#define IRQ_RX_TIMEOUT_MASK      0x80
#define IRQ_RX_DONE_MASK         0x40
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_VALID_HEADER_MASK    0x10
#define IRQ_TX_DONE_MASK         0x08
#define IRQ_CAD_DONE_MASK        0x04
#define IRQ_FHSS_CHANGE_CHANNEL_MASK 0x02
#define IRQ_CAD_DETECTED_MASK    0x01

/* Modes */
#define MODE_SLEEP               0x00
#define MODE_STANDBY             0x01
#define MODE_FSTX                0x02
#define MODE_TX                  0x03
#define MODE_FSRX                0x04
#define MODE_RXCONTINUOUS        0x05
#define MODE_RXSINGLE            0x06
#define MODE_CAD                 0x07
#define MODE_LORA                0x80
#define MODE_LOW_FREQ            0x08

/* ── Globals ─────────────────────────────────────────────────────────────── */
TaskHandle_t lora_task_handle = NULL;
volatile bool master_absent = false;

static const char *TAG = "radio";
static spi_device_handle_t s_spi = NULL;
static uint8_t s_node_id = 0;
static QueueHandle_t s_rx_queue = NULL;
static QueueHandle_t s_tx_queue = NULL;
static int16_t s_last_rx_msg_id = -1;

/* ── SPI Helpers ─────────────────────────────────────────────────────────── */
static void sx1278_write_reg(uint8_t reg, uint8_t val) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA;
    t.length = 16;
    t.tx_data[0] = reg | 0x80;
    t.tx_data[1] = val;
    spi_device_polling_transmit(s_spi, &t);
}

static uint8_t sx1278_read_reg(uint8_t reg) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.length = 16;
    t.tx_data[0] = reg & 0x7F;
    t.tx_data[1] = 0x00;
    spi_device_polling_transmit(s_spi, &t);
    return t.rx_data[1];
}

static void sx1278_write_fifo(const uint8_t *buf, size_t len) {
    uint8_t tx_buf[256];
    tx_buf[0] = REG_FIFO | 0x80;
    memcpy(tx_buf + 1, buf, len);

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (len + 1) * 8;
    t.tx_buffer = tx_buf;
    spi_device_polling_transmit(s_spi, &t);
}

static void sx1278_read_fifo(uint8_t *buf, size_t len) {
    uint8_t tx_buf[256] = {0};
    uint8_t rx_buf[256] = {0};
    tx_buf[0] = REG_FIFO & 0x7F;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (len + 1) * 8;
    t.tx_buffer = tx_buf;
    t.rx_buffer = rx_buf;
    spi_device_polling_transmit(s_spi, &t);
    memcpy(buf, rx_buf + 1, len);
}

/* ── ISR ─────────────────────────────────────────────────────────────────── */
static void IRAM_ATTR dio0_isr_handler(void *arg) {
    BaseType_t higher_prio_woken = pdFALSE;
    if (lora_task_handle) {
        vTaskNotifyGiveFromISR(lora_task_handle, &higher_prio_woken);
    }
    if (higher_prio_woken) {
        portYIELD_FROM_ISR();
    }
}

/* ── Internal State Changes ──────────────────────────────────────────────── */
static void sx1278_start_rx(void) {
    sx1278_write_reg(REG_DIO_MAPPING_1, 0x00); // DIO0=RxDone
    sx1278_write_reg(REG_FIFO_ADDR_PTR, sx1278_read_reg(REG_FIFO_RX_BASE_ADDR));
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_RXCONTINUOUS);
}

static esp_err_t sx1278_send(const radio_pkt_t *pkt) {
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_STANDBY);
    sx1278_write_reg(REG_FIFO_ADDR_PTR, sx1278_read_reg(REG_FIFO_TX_BASE_ADDR));
    
    uint8_t buf[256];
    size_t len = pkt_serialize(pkt, buf, sizeof(buf));
    if (len == 0) {
        ESP_LOGE(TAG, "pkt_serialize failed");
        return ESP_FAIL;
    }
    
    sx1278_write_fifo(buf, len);
    sx1278_write_reg(REG_PAYLOAD_LENGTH, len);
    sx1278_write_reg(REG_DIO_MAPPING_1, 0x40); // DIO0=TxDone
    sx1278_write_reg(REG_IRQ_FLAGS, 0xFF);
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_TX);
    
    return ESP_OK;
}

/* ── API ─────────────────────────────────────────────────────────────────── */
esp_err_t radio_init(uint8_t node_id, QueueHandle_t rx_q, QueueHandle_t tx_q) {
    s_node_id = node_id;
    s_rx_queue = rx_q;
    s_tx_queue = tx_q;

    // 1. SPI init
    spi_bus_config_t buscfg = {
        .miso_io_num = RADIO_PIN_MISO,
        .mosi_io_num = RADIO_PIN_MOSI,
        .sclk_io_num = RADIO_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return ESP_FAIL;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8000000, // 8 MHz
        .mode = 0,                 // SPI mode 0
        .spics_io_num = RADIO_PIN_NSS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return ESP_FAIL;
    }

    // 2. Hardware reset
    gpio_config_t rst_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << RADIO_PIN_RST),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&rst_conf);

    gpio_set_level(RADIO_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RADIO_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Verify chip
    uint8_t ver = sx1278_read_reg(REG_VERSION);
    if (ver != 0x12) {
        ESP_LOGE(TAG, "SX1278 not found! version=0x%02x", ver);
        return ESP_ERR_NOT_FOUND;
    }

    // 4. Configure LoRa mode
    sx1278_write_reg(REG_OP_MODE, MODE_SLEEP | MODE_LOW_FREQ);
    vTaskDelay(pdMS_TO_TICKS(10));
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_STANDBY);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 5. Frequency: 433 MHz -> Frf = 0x6C4000
    sx1278_write_reg(REG_FRF_MSB, 0x6C);
    sx1278_write_reg(REG_FRF_MID, 0x40);
    sx1278_write_reg(REG_FRF_LSB, 0x00);

    // 6. TX Power via PA_BOOST
    sx1278_write_reg(REG_PA_CONFIG, 0xFF); // PaSelect=1, MaxPower=7, OutputPower=15
    sx1278_write_reg(REG_PA_DAC, 0x87);    // enable +20dBm

    // 7. Modem Configuration
    sx1278_write_reg(REG_MODEM_CONFIG_1, 0x72); // BW=125kHz, CR=4/5, Explicit header
    sx1278_write_reg(REG_MODEM_CONFIG_2, 0x74); // SF7, CRC On, SymbTimeout MSB=0
    sx1278_write_reg(REG_MODEM_CONFIG_3, 0x04); // AgcAutoOn=1, LowDataRateOptimize=0
    sx1278_write_reg(REG_SYMB_TIMEOUT_LSB, 0x08); // 8 symbols RX timeout
    sx1278_write_reg(REG_PREAMBLE_MSB, 0x00);
    sx1278_write_reg(REG_PREAMBLE_LSB, 0x08);     // 8 symbol preamble
    sx1278_write_reg(REG_SYNC_WORD, 0x12);        // Private network

    sx1278_write_reg(REG_FIFO_TX_BASE_ADDR, 0x80);
    sx1278_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    sx1278_write_reg(REG_DIO_MAPPING_1, 0x00); // DIO0 = RxDone initially

    // 8. Configure DIO0 ISR
    gpio_config_t dio_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << RADIO_PIN_DIO0),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&dio_conf);

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed");
        return ESP_FAIL;
    }
    gpio_isr_handler_add(RADIO_PIN_DIO0, dio0_isr_handler, NULL);

    ESP_LOGI(TAG, "SX1278 init successful");
    return ESP_OK;
}

void lora_task(void *arg) {
    ESP_LOGI(TAG, "lora_task started");
    sx1278_start_rx();

    while (1) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_ABSENT_TIMEOUT_MS));
        
        if (notified == 0) {
            master_absent = true;
            ESP_LOGW(TAG, "master absent");
            sx1278_start_rx();
            continue;
        }

        uint8_t irq = sx1278_read_reg(REG_IRQ_FLAGS);
        sx1278_write_reg(REG_IRQ_FLAGS, 0xFF);

        if (irq == 0) {
            ESP_LOGD(TAG, "spurious DIO0 edge");
            sx1278_start_rx();
            continue;
        }

        if (irq & IRQ_TX_DONE_MASK) {
            sx1278_start_rx();
            continue;
        }

        if (irq & IRQ_RX_DONE_MASK) {
            master_absent = false;

            if (irq & IRQ_PAYLOAD_CRC_ERROR_MASK) {
                ESP_LOGW(TAG, "CRC error");
                sx1278_start_rx();
                continue;
            }

            uint8_t len = sx1278_read_reg(REG_RX_NB_BYTES);
            uint8_t current_addr = sx1278_read_reg(REG_FIFO_RX_CURRENT_ADDR);
            sx1278_write_reg(REG_FIFO_ADDR_PTR, current_addr);

            uint8_t buf[256];
            sx1278_read_fifo(buf, len);

            radio_pkt_t pkt;
            if (!pkt_deserialize(buf, len, &pkt)) {
                ESP_LOGE(TAG, "pkt_deserialize failed");
                sx1278_start_rx();
                continue;
            }

            int rssi = -157 + sx1278_read_reg(REG_PKT_RSSI_VALUE);
            int8_t snr_val = (int8_t)sx1278_read_reg(REG_PKT_SNR_VALUE);
            float snr = snr_val / 4.0f;

            ESP_LOGD(TAG, "RX type=0x%02x len=%d RSSI=%d SNR=%.2f", pkt.type, len, rssi, snr);

            switch (pkt.type) {
                case PKT_POLL:
                    if (pkt.node_id != s_node_id) {
                        sx1278_start_rx();
                        continue;
                    }

                    if (pkt.msg_id == s_last_rx_msg_id) {
                        ESP_LOGD(TAG, "Duplicate msg_id=%d", pkt.msg_id);
                    } else {
                        s_last_rx_msg_id = pkt.msg_id;
                        if (pkt.payload_len > 0) {
                            if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
                                ESP_LOGW(TAG, "lora_rx_queue full, dropping packet");
                            }
                        }
                    }

                    radio_pkt_t reply;
                    if (xQueueReceive(s_tx_queue, &reply, 0) == pdTRUE) {
                        reply.ack_id = pkt.msg_id;
                        if (sx1278_send(&reply) != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to send queued msg, sending empty ACK");
                            radio_pkt_t ack;
                            pkt_make_ack(&ack, s_node_id, pkt.msg_id);
                            sx1278_send(&ack);
                        }
                    } else {
                        radio_pkt_t ack;
                        pkt_make_ack(&ack, s_node_id, pkt.msg_id);
                        sx1278_send(&ack);
                    }
                    break;

                default:
                    ESP_LOGD(TAG, "unexpected pkt type 0x%02x", pkt.type);
                    sx1278_start_rx();
                    break;
            }
            continue;
        }

        if (irq & IRQ_RX_TIMEOUT_MASK) {
            sx1278_start_rx();
            continue;
        }
    }
}
