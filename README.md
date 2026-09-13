# WiBiDiB2 — BiDiB ↔ WiThrottle Gateway for Raspberry Pi Pico 2W

WiBiDiB2 is a model railroad control gateway that bridges **BiDiB** (the model railroad bus protocol) and **WiThrottle** (the WiFi throttle protocol used by apps like Engine Driver). It runs on a **Raspberry Pi Pico 2W** and uses the onboard WiFi.

## Features

- **BiDiB Interface** — 9-bit UART via PIO at 500 kbaud (GP18 TX, GP19 RX, GP6 DE/RE)
- **FreeRTOS** — kernel scheduling with dedicated tasks for BiDiB parsing, network I/O, log output, LED control, and background flash writes
- **WiFi Station (default)** — joins an existing WiFi network, IP via DHCP
- **WiFi Access Point (fallback)** — hosts its own network if the STA connection fails, static IP 192.168.4.1 with built-in DHCP server
- **WiThrottle TCP Server** — port 5550, up to 4 concurrent throttles
- **mDNS Discovery** — advertises `WiBiDiB._withrottle._tcp.local.` so Engine Driver finds the gateway automatically (STA and AP modes)
- **BiDiB Features** — `MSG_FEATURE_GETALL`/`GETNEXT`/`GET`/`SET` support with streaming (STRING_SIZE, STRING_DEBUG, FW_UPDATE_MODE, RELEVANT_PID_BITS)
- **User Strings** — `MSG_STRING_GET` returns the vendor (`WiBiDiB2`) and user (`Cool WiBiDiB2`) strings
- **Distributed Control** — BiDiB guest subscription/send support (DCCgen target mode)
- **Heartbeat Monitoring** — 10-second timeout with emergency stop
- **Non-blocking logging** — ring buffer drained to UART (debug probe bridge) via a dedicated FreeRTOS task
- **External Flash Storage** — W25Q32VFSIG (4 MB SPI NOR) on SPI1 for the persistent roster and user string; SPI transfers do not block the BiDiB PIO ISRs
- **HTTP Server (port 8080)** — JMRI-compatible roster XML for EngineDriver plus a web UI (`/roster.html`) to view, add, edit, and delete roster entries
- **Flash-backed roster** — up to 32 locomotives stored in external SPI flash, 4-slot LRU cache in RAM; edits persist across reboots
- **Status LED** — onboard CYW43 LED indicates state: fast blink (boot), slow blink (WiFi connecting), solid (ready), sticky fast blink (flash-write error)

## Hardware Requirements

- Raspberry Pi Pico 2W
- RS-485 transceiver (e.g., MAX485) for BiDiB bus connection:
  - Pico GP18 → DI (driver input)
  - Pico GP19 → RO (receiver output)
  - Pico GP6  → DE/RE (driver/receiver enable)
- W25Q32VFSIG SPI flash (**required for the persistent roster**) — 32 Mbit (4 MB) SPI NOR in SOIC-8, wired to SPI1:
  - Pico GP10 → CLK (W25Q32 pin 6)
  - Pico GP11 → DI/MOSI (W25Q32 pin 5)
  - Pico GP12 → DO/MISO (W25Q32 pin 2)
  - Pico GP13 → CS# (W25Q32 pin 1)
  - W25Q32 pin 8 → 3.3 V, pin 4 → GND; tie WP# (pin 3) and HOLD# (pin 7) to 3.3 V

If the flash chip is absent, the gateway still starts (BiDiB and WiThrottle work), but the roster is not persistent and edits will fail with a fast-blinking status LED.

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

| Define               | Default              | Description                              |
|----------------------|----------------------|------------------------------------------|
| `WIFI_SSID`          | `"myssid"`           | STA: WiFi network to join                |
| `WIFI_PASSWORD`      | `"mypassword"`       | STA: WiFi network password               |
| `WIFI_STA_TIMEOUT_MS`| `20000`              | STA connect/DHCP timeout before AP fallback |
| `WIFI_AP_SSID`       | `"myssid"`           | AP: fallback access point SSID           |
| `WIFI_AP_PASSWORD`   | `"mypassword"`       | AP: fallback access point password       |
| `AP_IP_ADDR`         | `"192.168.4.1"`      | Static IP of the AP                      |
| `WITHROTTLE_PORT`    | `5550`               | WiThrottle TCP port                      |
| `MAX_CLIENTS`        | `4`                  | Maximum simultaneous throttles           |
| `HEARTBEAT_TIMEOUT_S`| `10`                 | Heartbeat timeout in seconds             |

### Local network credentials

To keep your STA credentials out of source control, copy `include/network_config.example.h` to `include/network_config.h` (git-ignored) and set `WIFI_SSID` / `WIFI_PASSWORD` there. If the file exists it overrides the `config.h` defaults; the firmware builds fine without it.

## Flash Storage

The external W25Q32VFSIG (Winbond, 4 MB SPI NOR) is probed at boot on SPI1 (`flash_store_init()`). If the chip is missing or miswired, `flash_store_init()` returns false and the gateway continues without persistent storage.

- Reads and writes are **synchronous but IRQ-safe**: `spi_write/read_blocking()` keep interrupts enabled, so the BiDiB PIO ISRs (priority 4) keep draining the RX FIFO during transfers. Unlike onboard XIP flash writes, external SPI does **not** stall the system.
- All roster writes are dispatched to a dedicated FreeRTOS task (`roster_writer_task`, priority 1), so the ~200 ms per-sector SPI stall never blocks lwIP callbacks, WiThrottle handling, or BiDiB bus processing.
- Writes preserve untouched data in the affected 4 KB sectors (read-modify-write) and program page-by-page (256 bytes).
- API: `flash_store_read()`, `flash_store_write()`, `flash_store_erase_all()`, `flash_store_jedec_id()`.

### Storage layout

| Address | Content |
|---------|---------|
| `0x000000` | BiDiB user string (24 bytes) |
| `0x001000` | Roster header (magic, version, used-slot bitmap, CRC32) |
| `0x002000` .. `0x021000` | Roster entry slots 0..31 (one 4 KB sector per entry) |

On first boot with blank flash, the header is invalid and the firmware writes a default three-entry roster (BR 185, BR 01, UP 4014) to flash.

## Roster and HTTP Web UI

The gateway serves an HTTP interface on **port 8080** for both machine-readable (JMRI XML) and human-editable roster access.

| URL | Purpose |
|-----|---------|
| `http://<gateway>:8080/` | Redirects to `/roster.html` |
| `http://<gateway>:8080/roster.html` | Web UI: table of locomotives with Add / Edit / Delete buttons |
| `http://<gateway>:8080/roster/` | JMRI-compatible roster XML — used automatically by Engine Driver |
| `http://<gateway>:8080/edit?slot=N` | Edit form for slot N (or `slot=new` for a new entry) |

The roster supports up to **32 entries** in flash. Only 4 entries are kept in RAM at a time (LRU cache). Each entry stores:

- id, road name, road number, manufacturer, model
- DCC address (short or long), max speed, max function number
- Per-function label, lockable flag, and visible flag (F0..F28)

Save/delete operations return a redirect immediately; the flash write happens in the background. If a flash write fails, the RAM cache is rolled back to match what is actually persisted and the onboard LED switches to a sticky fast-blink error state.

## Status LED

The onboard CYW43 LED indicates gateway state:

| Pattern | Meaning |
|---------|---------|
| Fast blink | Boot / init in progress |
| Slow blink | WiFi connecting |
| Solid on | Ready |
| Off | WiFi failed (gateway still runs BiDiB) |
| Sticky fast blink | Flash write failed — roster in RAM has been rolled back |

## Protocol

- **WiThrottle** — standard protocol as used by JMRI WiThrottle / Engine Driver
- **mDNS** — service `WiBiDiB._withrottle._tcp.local.`, port 5550; the gateway is discovered automatically by Engine Driver, no manual IP/port entry needed
- **BiDiB** — protocol version 0.8, distributed control (rev 1.29) for DCCgen target mode

## Project Structure

```
WiBiDiB2/
├── main.c                        # Entry point, FreeRTOS task creation, scheduler start
├── bidib.c                       # BiDiB PIO protocol (ISR-driven)
├── bidib_uart.pio                # PIO assembly (9-bit UART, 500k baud)
├── tcp_server.c                  # WiFi (STA + AP fallback) + WiThrottle TCP server
├── withrottle_if.c               # WiThrottle message processing
├── smartphone_if.c               # Throttle table management
├── bidib_client_parser.c         # BiDiB client message parser
├── bidib_client_if.c             # BiDiB client interface (RX stream buffer, TX spinlock)
├── crc_8bit.c                    # CRC-8 for BiDiB frames
├── log.c                         # Non-blocking ring-buffer logging (UART)
├── led.c                         # Onboard LED state machine (FreeRTOS task)
├── mdns.c                        # mDNS responder (_withrottle._tcp)
├── flash_store.c                 # W25Q32VFSIG external SPI flash driver
├── roster.c                      # Flash-backed roster with 4-slot LRU cache
├── roster_writer.c               # Background flash-write task + queue
├── http_server.c                 # HTTP router: /, /roster.html, /roster/, /edit, /save, /delete
├── form_parser.c                 # application/x-www-form-urlencoded parser
├── dhcpserver/                   # DHCP server (from pico-examples)
├── include/                      # Header files
│   ├── FreeRTOSConfig.h          # FreeRTOS kernel configuration (RP2350)
│   ├── config.h
│   ├── network_config.example.h  # Template for local STA credentials
│   ├── datatypes.h
│   ├── bidib.h
│   ├── bidib_messages.h          # Official BiDiB message definitions
│   ├── bidib_distributed_control.h
│   ├── features.h                # WiThrottle node feature table
│   ├── log.h
│   ├── led.h
│   ├── mdns.h
│   ├── flash_store.h             # Flash pin config + API + roster layout constants
│   ├── roster.h                  # Roster data model + public API
│   ├── roster_writer.h           # Background writer task interface
│   ├── http_server.h
│   ├── form_parser.h
│   ├── tcp_server.h
│   ├── withrottle_if.h
│   ├── smartphone_if.h
│   ├── lwipopts.h
│   └── crc_8bit.h
├── CHANGES.md                    # Migration history (FreeRTOS + roster editor)
├── CMakeLists.txt
└── pico_sdk_import.cmake
```

## FreeRTOS Task Layout

| Task | Priority | Stack | Function |
|------|----------|-------|----------|
| BiDiB parser | 4 | 8 KB | `run_bidib_client()` loop |
| CYW43 async (pico-sdk) | 3 | 8 KB | WiFi driver events |
| Network init | 2 | 6 KB | WiFi/TCP/roster/HTTP/smartphone init |
| lwIP tcpip | 1 | 8 KB | HTTP + WiThrottle callbacks |
| Log output | 1 | 2 KB | UART drain |
| Roster writer | 1 | 1 KB | Background flash writes |
| LED | 1 | 1 KB | Status LED state machine |

## License

This project uses the BiDiB protocol headers from [bidib.org](http://www.bidib.org) and the DHCP server from Raspberry Pi Pico Examples. See individual file headers for license terms.
