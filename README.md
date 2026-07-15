# WiBiDiB2 — BiDiB ↔ WiThrottle Gateway for Raspberry Pi Pico 2W

WiBiDiB2 is a model railroad control gateway that bridges **BiDiB** (the model railroad bus protocol) and **WiThrottle** (the WiFi throttle protocol used by apps like Engine Driver). It runs on a **Raspberry Pi Pico 2W** and uses the onboard WiFi to act as an Access Point.

## Features

- **BiDiB Interface** — 9-bit UART via PIO at 500 kbaud (GP18 TX, GP19 RX, GP6 DE/RE)
- **WiFi Access Point** — configurable SSID/password, static IP 192.168.4.1
- **WiThrottle TCP Server** — port 5550, up to 4 concurrent throttles
- **DHCP Server** — built-in for clients connecting to the AP
- **Distributed Control** — BiDiB guest subscription/send support (DCCgen target mode)
- **Heartbeat Monitoring** — 10-second timeout with emergency stop

## Hardware Requirements

- Raspberry Pi Pico 2W
- RS-485 transceiver (e.g., MAX485) for BiDiB bus connection:
  - Pico GP18 → DI (driver input)
  - Pico GP19 → RO (receiver output)
  - Pico GP6  → DE/RE (driver/receiver enable)

## Building

### Prerequisites

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) v2.2.0
- CMake ≥ 3.13
- ARM GCC toolchain (GCC 14.2 Rel1 or compatible)
- Raspberry Pi Pico VS Code Extension (recommended)

### Build Steps

```bash
# Clone the repo
git clone <repo-url> WiBiDiB2
cd WiBiDiB2

# Configure (set PICO_SDK_PATH or let the SDK import.cmake locate it)
cmake -B build -DPICO_BOARD=pico2_w

# Build
cmake --build build
```

Output files (in `build/`):
- `WiBiDiB.uf2` — UF2 firmware for drag-and-drop flashing
- `WiBiDiB.elf` — ELF executable
- `WiBiDiB.hex` — Intel HEX
- `WiBiDiB.bin` — Raw binary

### Flashing

1. Hold the BOOTSEL button on the Pico 2W while connecting USB.
2. Copy `build/WiBiDiB.uf2` to the mounted RPI-RP2 drive.

### VS Code (alternative)

Open the project folder in VS Code with the Raspberry Pi Pico Extension installed. Use the **"Compile Project in RAM"** or **"Compile Project"** commands from the extension's status bar.

## Configuration

Edit `include/config.h`:

| Define               | Default          | Description                    |
|----------------------|------------------|--------------------------------|
| `WIFI_SSID`          | `"myssid"`       | WiFi AP SSID                   |
| `WIFI_PASSWORD`      | `"mypassword"`   | WiFi AP password               |
| `AP_IP_ADDR`         | `"192.168.4.1"`  | Static IP of the AP            |
| `WITHROTTLE_PORT`    | `5550`           | WiThrottle TCP port            |
| `MAX_CLIENTS`        | `4`              | Maximum simultaneous throttles |
| `HEARTBEAT_TIMEOUT_S`| `10`             | Heartbeat timeout in seconds   |

## Protocol

- **WiThrottle** — standard protocol as used by JMRI WiThrottle / Engine Driver
- **BiDiB** — protocol version 0.8, distributed control (rev 1.29) for DCCgen target mode

## Project Structure

```
WiBiDiB2/
├── main.c                        # Entry point & main loop
├── bidib.c                       # BiDiB PIO protocol (ISR-driven)
├── bidib_uart.pio                # PIO assembly (9-bit UART, 500k baud)
├── tcp_server.c                  # WiFi AP + TCP server
├── withrottle_if.c               # WiThrottle message processing
├── smartphone_if.c               # Throttle table management
├── bidib_client_parser.c         # BiDiB client message parser
├── bidib_client_if.c             # BiDiB client interface
├── crc_8bit.c                    # CRC-8 for BiDiB frames
├── dhcpserver/                   # DHCP server (from pico-examples)
├── include/                      # Header files
│   ├── config.h
│   ├── datatypes.h
│   ├── bidib.h
│   ├── bidib_messages.h          # Official BiDiB message definitions
│   ├── bidib_distributed_control.h
│   ├── tcp_server.h
│   ├── withrottle_if.h
│   ├── smartphone_if.h
│   ├── lwipopts.h
│   └── crc_8bit.h
├── CMakeLists.txt
└── pico_sdk_import.cmake
```

## License

This project uses the BiDiB protocol headers from [bidib.org](http://www.bidib.org) and the DHCP server from Raspberry Pi Pico Examples. See individual file headers for license terms.
