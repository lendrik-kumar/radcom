#include "radio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <time.h>
#include <syslog.h>
#include <gpiod.h>

/* ── Register map (mirrors ESP32 radio.c exactly) ── */
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_PA_DAC               0x4D
#define REG_OCP                  0x0B
#define REG_LNA                  0x0C
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE_ADDR    0x0E
#define REG_FIFO_RX_BASE_ADDR    0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS_MASK       0x11
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_SYMB_TIMEOUT_LSB     0x1F
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH       0x22
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYNC_WORD            0x39
#define REG_DIO_MAPPING_1        0x40
#define REG_VERSION              0x42

/* IRQ flag bits — same names as ESP32 firmware */
#define IRQ_RX_TIMEOUT_MASK      0x80
#define IRQ_RX_DONE_MASK         0x40
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_TX_DONE_MASK         0x08

/* Op-mode bits */
#define MODE_LORA                0x80
#define MODE_LOW_FREQ            0x08
#define MODE_SLEEP               0x00
#define MODE_STANDBY             0x01
#define MODE_TX                  0x03
#define MODE_RXCONTINUOUS        0x05

static int spi_fd  = -1;
static int dio0_fd = -1;

static void delay_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── GPIO via libgpiod ── */
static struct gpiod_line *line_rst = NULL;
static struct gpiod_line *line_dio0 = NULL;
static struct gpiod_line *line_nss = NULL;

static int gpio_setup_done = 0;
static int gpio_setup(void) {
    if (gpio_setup_done) return 0;
    line_rst = gpiod_line_find("GPIO25");
    if (!line_rst) { syslog(LOG_CRIT, "gpiod_line_find GPIO25 failed"); return -1; }
    
    line_dio0 = gpiod_line_find("GPIO23");
    if (!line_dio0) { syslog(LOG_CRIT, "gpiod_line_find GPIO23 failed"); return -1; }

    line_nss = gpiod_line_find("GPIO22");
    if (!line_nss) { syslog(LOG_CRIT, "gpiod_line_find GPIO22 failed"); return -1; }

    if (gpiod_line_request_output(line_rst, "radcom", 1) < 0) {
        syslog(LOG_CRIT, "gpiod_line_request_output rst failed");
    }

    if (gpiod_line_request_output(line_nss, "radcom", 1) < 0) {
        syslog(LOG_CRIT, "gpiod_line_request_output nss failed");
    }

    if (gpiod_line_request_rising_edge_events(line_dio0, "radcom") < 0) {
        syslog(LOG_CRIT, "gpiod_line_request_rising_edge_events dio0 failed");
    }
    
    dio0_fd = gpiod_line_event_get_fd(line_dio0);
    if (dio0_fd < 0) {
        syslog(LOG_CRIT, "gpiod_line_event_get_fd failed");
        return -1;
    } else {
        int flags = fcntl(dio0_fd, F_GETFL, 0);
        fcntl(dio0_fd, F_SETFL, flags | O_NONBLOCK);
    }
    gpio_setup_done = 1;
    return 0;
}

static void gpio_set_value(int pin, int v) {
    if (pin == 25 && line_rst) {
        gpiod_line_set_value(line_rst, v);
    }
}

/* Drain all pending libgpiod events (non-blocking) */
static void gpio_clear_events(void) {
    struct gpiod_line_event ev;
    while (gpiod_line_event_read(line_dio0, &ev) == 0) {}
}

/* ── SPI helpers ── */
static void sx1278_write_reg(uint8_t reg, uint8_t val) {
    if (line_nss) gpiod_line_set_value(line_nss, 0);
    uint8_t tx[2] = { (uint8_t)(reg | 0x80), val };
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = 0,
        .len = 2, .speed_hz = 1000000, .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    if (line_nss) gpiod_line_set_value(line_nss, 1);
}

static uint8_t sx1278_read_reg(uint8_t reg) {
    if (line_nss) gpiod_line_set_value(line_nss, 0);
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), 0x00 };
    uint8_t rx[2] = { 0, 0 };
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 2, .speed_hz = 1000000, .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    if (line_nss) gpiod_line_set_value(line_nss, 1);
    return rx[1];
}

static void sx1278_write_fifo(const uint8_t *buf, size_t len) {
    if (line_nss) gpiod_line_set_value(line_nss, 0);
    uint8_t tx[257];
    tx[0] = REG_FIFO | 0x80;
    memcpy(tx + 1, buf, len);
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = 0,
        .len = (uint32_t)(len + 1), .speed_hz = 1000000, .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    if (line_nss) gpiod_line_set_value(line_nss, 1);
}

static void sx1278_read_fifo(uint8_t *buf, size_t len) {
    if (line_nss) gpiod_line_set_value(line_nss, 0);
    uint8_t tx[257] = {0};
    uint8_t rx[257] = {0};
    tx[0] = REG_FIFO & 0x7F; /* read register */
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = (uint32_t)(len + 1), .speed_hz = 1000000, .bits_per_word = 8,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
    if (line_nss) gpiod_line_set_value(line_nss, 1);
    memcpy(buf, rx + 1, len);
}

/* ── Internal: put radio into continuous RX, DIO0=RxDone ── */
static void sx1278_start_rx_internal(void) {
    sx1278_write_reg(REG_DIO_MAPPING_1, 0x00); /* DIO0 = RxDone */
    sx1278_write_reg(REG_FIFO_ADDR_PTR, sx1278_read_reg(REG_FIFO_RX_BASE_ADDR));
    sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_RXCONTINUOUS);
}

/* ── Public API ── */
int radio_init(void) {
    spi_fd = open(SPI_DEVICE, O_RDWR);
    if (spi_fd < 0) { syslog(LOG_CRIT, "open %s: %m", SPI_DEVICE); return -1; }
    uint8_t mode = 0; uint32_t speed = 1000000;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    if (gpio_setup() < 0) return -1;

    /* Hardware reset: RST low 10ms, high 10ms (matches ESP32 init) */
    gpio_set_value(25, 0); delay_ms(10);
    gpio_set_value(25, 1); delay_ms(10);
    gpio_set_value(PIN_RST, 1); delay_ms(10);

    uint8_t ver = sx1278_read_reg(REG_VERSION);
    if (ver != 0x12) {
        syslog(LOG_CRIT, "SX1278 not found! version=0x%02x (expected 0x12)", ver);
        return -1;
    }

    /* Force into LoRa mode — it MUST be in SLEEP first */
    int lora_ok = 0;
    for (int i = 0; i < 10; i++) {
        /* 1. Enter FSK SLEEP with LowFreq on */
        sx1278_write_reg(REG_OP_MODE, MODE_SLEEP | MODE_LOW_FREQ);
        delay_ms(10);
        
        /* 2. Enable LoRa mode WHILE STILL IN SLEEP */
        sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_SLEEP);
        delay_ms(10);
        
        /* 3. Now it is safe to enter STANDBY */
        sx1278_write_reg(REG_OP_MODE, MODE_LORA | MODE_LOW_FREQ | MODE_STANDBY);
        delay_ms(10);
        
        uint8_t op = sx1278_read_reg(REG_OP_MODE);
        if (op == (MODE_LORA | MODE_LOW_FREQ | MODE_STANDBY)) {
            lora_ok = 1;
            break;
        }
        syslog(LOG_WARNING, "radio_init: failed to enter LoRa mode (OP=0x%02x), retrying...", op);
    }
    if (!lora_ok) {
        syslog(LOG_CRIT, "radio_init: fatal: chip stuck in FSK mode!");
        return -1;
    }

    /* Frequency: 433 MHz → Frf = 433e6 / (32e6/2^19) = 0x6C4000 */
    sx1278_write_reg(REG_FRF_MSB, 0x6C);
    sx1278_write_reg(REG_FRF_MID, 0x40);
    sx1278_write_reg(REG_FRF_LSB, 0x00);

    /* TX via PA_BOOST (Ra-02 wired to PA_BOOST) */
    sx1278_write_reg(REG_PA_CONFIG, 0x80); /* PaSelect=1, MaxPower=4, OutputPower=0 (+2dBm total, absolute minimum current) */
    sx1278_write_reg(REG_PA_DAC,    0x84); /* Default PA_DAC, no +20dBm boost */

    /* Modem config — identical to ESP32 node */
    sx1278_write_reg(REG_MODEM_CONFIG_1,  0x72); /* BW=125kHz, CR=4/5, explicit header */
    sx1278_write_reg(REG_MODEM_CONFIG_2,  0x74); /* SF7, CRC on, SymbTimeout MSB=0 */
    sx1278_write_reg(REG_MODEM_CONFIG_3,  0x04); /* AgcAutoOn, LowDataRateOptimize=0 */
    sx1278_write_reg(REG_SYMB_TIMEOUT_LSB, 0x08);
    sx1278_write_reg(REG_PREAMBLE_MSB,    0x00);
    sx1278_write_reg(REG_PREAMBLE_LSB,    0x08); /* 8-symbol preamble */
    sx1278_write_reg(REG_SYNC_WORD,       0x12); /* private network */

    /* FIFO: TX at 0x80, RX at 0x00 (same as ESP32 node) */
    sx1278_write_reg(REG_FIFO_TX_BASE_ADDR, 0x80);
    sx1278_write_reg(REG_FIFO_RX_BASE_ADDR, 0x00);
    sx1278_write_reg(REG_DIO_MAPPING_1,     0x00); /* DIO0 = RxDone initially */

    

    syslog(LOG_NOTICE, "SX1278 initialized (ver=0x12, 433 MHz, SF7, BW125)");
    return 0;
}

void radio_start_rx(void) {
    sx1278_start_rx_internal();
}

int radio_send(const radio_pkt_t *pkt) {
    uint8_t buf[256];
    size_t len = pkt_serialize(pkt, buf, sizeof(buf));
    if (len == 0) { syslog(LOG_ERR, "radio_send: pkt_serialize failed"); return -1; }

    uint8_t current_op = sx1278_read_reg(REG_OP_MODE);
    if ((current_op & MODE_LORA) == 0) {
        syslog(LOG_CRIT, "radio_send: chip browned-out (OP=0x%02x)! Auto-recovering...", current_op);
        if (radio_init() < 0) return -1;
    }

    sx1278_write_reg(REG_OP_MODE,       MODE_LORA | MODE_LOW_FREQ | MODE_STANDBY);
    sx1278_write_reg(REG_FIFO_ADDR_PTR, sx1278_read_reg(REG_FIFO_TX_BASE_ADDR));
    sx1278_write_fifo(buf, len);
    sx1278_write_reg(REG_PAYLOAD_LENGTH,  (uint8_t)len);
    sx1278_write_reg(REG_DIO_MAPPING_1,   0x40); /* DIO0 = TxDone */
    sx1278_write_reg(REG_IRQ_FLAGS,       0xFF); /* clear all flags */
    gpio_clear_events(); /* Drain any stale events BEFORE entering TX */
    sx1278_write_reg(REG_OP_MODE,         MODE_LORA | MODE_LOW_FREQ | MODE_TX);

    /* Block until TxDone (DIO0 rising edge), max 3 seconds */
    uint32_t start_time = (uint32_t)time(NULL);
    while (1) {
        uint32_t elapsed = (uint32_t)time(NULL) - start_time;
        if (elapsed >= 3) {
            int lvl = gpiod_line_get_value(line_dio0);
            uint8_t flags = sx1278_read_reg(REG_IRQ_FLAGS);
            uint8_t op = sx1278_read_reg(REG_OP_MODE);
            syslog(LOG_WARNING, "radio_send: TxDone timeout, DIO0=%d IRQ=0x%02x OP=0x%02x", lvl, flags, op);
            sx1278_write_reg(REG_IRQ_FLAGS, 0xFF);
            sx1278_start_rx_internal();
            return -1;
        }

        struct pollfd pfd = { .fd = dio0_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, (3 - elapsed) * 1000);
        if (ret <= 0) continue;

        gpio_clear_events(); /* Consume the event we just woke up for */
        
        uint8_t irq = sx1278_read_reg(REG_IRQ_FLAGS);
        if (irq & IRQ_TX_DONE_MASK) {
            sx1278_write_reg(REG_IRQ_FLAGS, IRQ_TX_DONE_MASK);
            break; /* Successfully finished TX */
        }
        
        if (irq == 0) {
            syslog(LOG_DEBUG, "radio_send: ignoring spurious DIO0 edge");
        } else {
            syslog(LOG_DEBUG, "radio_send: unexpected IRQ during TX: 0x%02x", irq);
            sx1278_write_reg(REG_IRQ_FLAGS, irq);
        }
    }

    /* Return to RX standby — DIO0 mapping back to RxDone */
    sx1278_start_rx_internal();
    return 0;
}

/*
 * Wait for a received packet. Blocks up to timeout_ms.
 * Returns 1 = packet received and deserialized OK.
 *         0 = timeout or spurious/CRC error (pkt untouched).
 *        -1 = fatal error.
 *
 * NOTE: radio must be in RXCONTINUOUS mode before calling.
 * Call radio_start_rx() first.
 */
int radio_wait_rx(radio_pkt_t *pkt, int timeout_ms) {
    struct pollfd pfd = { .fd = dio0_fd, .events = POLLIN };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)  { syslog(LOG_ERR, "poll: %m"); return -1; }
    if (ret == 0) return 0; /* timeout */

    gpio_clear_events(); /* Consume the event */

    uint8_t irq = sx1278_read_reg(REG_IRQ_FLAGS);
    sx1278_write_reg(REG_IRQ_FLAGS, 0xFF); /* clear all */

    if (irq == 0) {
        syslog(LOG_DEBUG, "radio_wait_rx: spurious DIO0 edge");
        return 0;
    }

    if (!(irq & IRQ_RX_DONE_MASK)) {
        /* Not RxDone — could be RxTimeout in single-shot mode, ignore */
        syslog(LOG_DEBUG, "radio_wait_rx: IRQ=0x%02x but no RxDone", irq);
        return 0;
    }

    if (irq & IRQ_PAYLOAD_CRC_ERROR_MASK) {
        syslog(LOG_WARNING, "radio_wait_rx: CRC error, discarding");
        return 0;
    }

    uint8_t len  = sx1278_read_reg(REG_RX_NB_BYTES);
    uint8_t addr = sx1278_read_reg(REG_FIFO_RX_CURRENT_ADDR);
    sx1278_write_reg(REG_FIFO_ADDR_PTR, addr);

    uint8_t buf[256];
    sx1278_read_fifo(buf, len);

    /* Log RSSI and SNR for diagnostics */
    int rssi = -157 + sx1278_read_reg(REG_PKT_RSSI_VALUE);
    int8_t snr_raw = (int8_t)sx1278_read_reg(REG_PKT_SNR_VALUE);
    syslog(LOG_DEBUG, "RX len=%d RSSI=%d SNR=%.1f", len, rssi, snr_raw / 4.0);

    /* pkt_deserialize returns true (non-zero) on success — matches ESP32 packet.c */
    if (!pkt_deserialize(buf, len, pkt)) {
        syslog(LOG_WARNING, "radio_wait_rx: pkt_deserialize failed (len=%d)", len);
        return 0;
    }

    return 1;
}
