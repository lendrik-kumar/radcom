# radcom — node firmware

Off-grid mesh messaging. ESP32-C3 + SX1278 (Ra-02) @ 433 MHz. ESP-IDF v5 + FreeRTOS.

## Wiring — ESP32-C3 mini → Ra-02 (SX1278)

| GPIO | Ra-02 Pin | Signal |
|---|---|---|
| GPIO 6 | SCK | SPI clock |
| GPIO 7 | MOSI | SPI data in |
| GPIO 2 | MISO | SPI data out |
| GPIO 10 | NSS | Chip select |
| GPIO 3 | RESET | Radio reset |
| GPIO 1 | DIO0 | TX/RX done IRQ |
| 3.3V | 3.3V | Power (3.3V ONLY) |
| GND | GND | Ground |

**Always attach a 433 MHz antenna before powering on.**

## Setup (macOS, fresh install)

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.3
./install.sh esp32c3
source ~/esp/esp-idf/export.sh   # add to ~/.zshrc

cd /path/to/radcom/firmware/node
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

## Provision node_id (one-time per node)

```bash
# Connect to "radcom" WiFi (pass: radcom2024), then:
curl -X POST http://192.168.4.1/config \
     -H "Content-Type: application/json" \
     -d '{"node_id": 1}'
```

## Run tests (no hardware needed)

```bash
cd firmware/tests && make run
# 43/43 tests pass
```
