# ESP32-C3 TCP-UART Bridge

High-performance transparent TCP-to-UART bridge for ESP32-C3.

Connects to a WiFi network in STA mode and exposes a TCP server.
Any bytes received on the TCP socket are forwarded to UART and vice versa.

## Features

- Event-driven UART RX (hardware interrupt, ~120 us latency at 250000 baud)
- TCP_NODELAY + SO_KEEPALIVE for minimal latency
- Single TCP client with automatic replacement on new connection
- IP-based access control (optional)
- 8 KB DMA-backed UART ring buffers
- Automatic WiFi reconnection
- Web UI for all settings (WiFi, UART, TCP) with NVS persistence
- OTA firmware updates via curl
- Configurable WiFi TX power
- LED status indicator
- Static IP support
- WiFi power save disabled (zero extra latency)
- Task watchdog on bridge tasks (auto-reboot on hang)
- Auto-reboot on prolonged WiFi loss (configurable)
- mDNS hostname (`uart-bridge.local` by default)
- WiFi RSSI and uptime monitoring on web UI

## Requirements

- ESP-IDF v5.4+
- ESP32-C3 board

## Install ESP-IDF

```bash
# 1. System packages
sudo apt-get install -y git wget flex bison gperf python3 python3-pip \
  python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util \
  libusb-1.0-0

# 2. Clone ESP-IDF
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32c3
```

## Build & Flash

```bash
# Activate ESP-IDF environment (run once per terminal session)
export IDF_PATH=~/esp/esp-idf
. $IDF_PATH/export.sh

# Build
cd ~/esp32/uart_bridge
idf.py set-target esp32c3       # only needed once or after fullclean
idf.py menuconfig               # set WiFi SSID/password for first boot
idf.py build

# Flash and monitor (replace PORT with your device, e.g. /dev/ttyACM0)
idf.py -p PORT flash monitor
```

## Configuration

### First boot

Set WiFi credentials via `idf.py menuconfig` before the first flash.
After booting, all settings can be changed via the web UI.

### Web UI

Open `http://<ESP_IP>:80` in a browser. The settings page allows changing:

- **WiFi**: SSID, password, TX power, static IP / DHCP
- **UART**: baud rate
- **Network**: TCP port, HTTP port, allowed client IP, mDNS hostname

Settings are saved to NVS flash and persist across reboots.
Use the "Reset to Defaults" button to revert to Kconfig compile-time values.

### menuconfig defaults

Kconfig values serve as defaults for the first boot (before NVS is populated):

| Parameter          | Default        | Description                         |
|--------------------|----------------|-------------------------------------|
| WiFi SSID          | myssid         | Network name                        |
| WiFi Password      | mypassword     | Network password                    |
| WiFi TX Power      | 34 (8.5 dBm)  | 0.25 dBm units, max 60 (15 dBm)      |
| Static IP          | (empty)        | Empty = DHCP                        |
| UART Baud Rate     | 250000         |                                     |
| UART TX GPIO       | 21             | GPIO pin for UART TX                |
| UART RX GPIO       | 20             | GPIO pin for UART RX                |
| TCP Port           | 3333           | TCP server listen port              |
| HTTP Port          | 80             | Web UI and OTA port                 |
| Allowed Client IP  | (empty)        | IP filter; empty = accept any       |
| Hostname           | uart-bridge    | mDNS name (.local)                  |
| WiFi Reboot Timeout| 5              | Reboot after N min without WiFi     |
| LED GPIO           | 8              | Status LED pin                      |

## OTA Firmware Update

After the first flash over USB, subsequent updates can be done over WiFi using `curl`:

```bash
# Build the firmware
idf.py build

# Flash over WiFi (replace <ESP_IP> with the device IP)
curl -X POST http://<ESP_IP>:80/update \
  --data-binary @build/uart_bridge.bin
```

The device validates the image, flashes to the inactive OTA partition,
and reboots automatically.

**Important:** the first flash must be done over USB since OTA
requires the two-OTA partition table to already be in place.

## LED Indicator

| State               | Pattern                          |
|---------------------|----------------------------------|
| WiFi connecting     | 2 blinks per second              |
| WiFi connected      | 50 ms flash every 5 seconds      |
| TCP client connected| 50 ms flash every 1 second       |
| Packet transfer     | Brief flash per packet           |


## Latency Test

A test script is included to measure bridge round-trip latency using standard G-code commands.

```bash
python3 test_bridge.py <ESP_IP> 3333
```

The test runs two phases:

1. **Command latency** -- measures round-trip time for various G-code commands (M118 echo, M105 temperatures, M114 position, M115 firmware info, etc.)
2. **Sustained load** -- sends continuous M118 pings for 60 seconds, reports latency distribution, p95/p99, and a timeline graph

### Example results (250000 baud, WiFi 2.4 GHz)

```
Sustained load test: 60s
  Requests:  8632 ok, 1 timeouts
  Rate:      143.9 req/s

  Min:     3.4 ms
  Max:     167.8 ms
  Median:  4.5 ms
  p95:     15.9 ms
  p99:     32.6 ms

  <5ms   │████████████████████████████████████████ 4705 (55%)
  5-10   │██████████████████████████ 3043 (35%)
  10-20  │████ 559 (6%)
  20-50  │██ 291 (3%)
  50-100 │ 22 (0%)
  >100ms │ 12 (0%)
```

90% of responses complete in under 10 ms -- comparable to wired USB-UART.

## Pin Connections

Default GPIO mapping for ESP32-C3:

```
ESP32-C3 GPIO21 (TX) --> MCU RX
ESP32-C3 GPIO20 (RX) <-- MCU TX
ESP32-C3 GND         --- MCU GND
```
