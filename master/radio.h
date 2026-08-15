#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "packet.h"

// Hardware settings matching node
#define LORA_FREQ_HZ     433000000UL
#define LORA_BW_KHZ      125
#define LORA_SF          7
#define LORA_CR          1
#define LORA_TX_DBM      20
#define LORA_PREAMBLE    8
#define LORA_SYNC_WORD   0x12

// Raspberry Pi SPI & GPIO mapping
#define SPI_DEVICE       "/dev/spidev0.0"
#define PIN_RST          25          // GPIO25
#define PIN_DIO0         24          // GPIO24

int radio_init(void);
void radio_start_rx(void);
int radio_send(const radio_pkt_t *pkt);
int radio_wait_rx(radio_pkt_t *pkt, int timeout_ms);
