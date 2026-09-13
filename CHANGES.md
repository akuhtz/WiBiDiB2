# CHANGES — FreeRTOS Migration

## Overview

Migrated WiBiDiB2 from bare-metal polling (`NO_SYS=1`) to FreeRTOS
(`NO_SYS=0`) on the RP2350 Cortex-M33. This converts lwIP from raw-poll
mode to threaded mode, runs the BiDiB parser and log output as dedicated
FreeRTOS tasks, and adds dual-core safety via hardware spinlocks.

**Branch:** `freertos`
**Binary impact (end of migration, after bss reduction):**
362 KB → 377 KB text (+15 KB), 92 KB → 96 KB bss (+4 KB).
Total ~473 KB — fits in Pico 2W's 520 KB SRAM.

---

## Files Changed

| File | Change |
|------|--------|
| `CMakeLists.txt` | FreeRTOS kernel import, new link targets, global CYW43 defines |
| `include/FreeRTOSConfig.h` | RP2350 kernel configuration |
| `include/lwipopts.h` | `NO_SYS` 1 → 0, `TCPIP_MBOX_SIZE`, `TCPIP_THREAD_STACKSIZE` |
| `include/lwipopts_examples_common.h` | `NO_SYS` 1 → 0 |
| `include/bidib.h` | Critical sections → FreeRTOS-aware |
| `include/bidib_client_if.h` | RX buffer externs removed, spinlock extern added |
| `bidib_client_if.c` | RX stream buffer, TX spinlock, `init_bidib_client_if_buffers()` |
| `bidib.c` | TX ISR + parser TX use spinlock, PIO ISR priorities 0 → 4 |
| `log.c` | Critical sections → FreeRTOS-aware |
| `main.c` | Tasks + scheduler, LED task, fault handlers with PSPLIM dump |
| `tcp_server.c` | `cyw43_arch_poll()` → `vTaskDelay()`, `wifi_init()` simplified |
| `http_server.c` | Fully resumable XML state machine (fixes ERR_MEM partial sends) |
| `include/led.h` | **New file** — LED state enum and API |
| `led.c` | **New file** — LED control task |

---

## Detailed Changes

### 1. Build System (`CMakeLists.txt`)

**Before:** Linked `pico_cyw43_arch_lwip_poll` (bare-metal lwIP polling).
**After:** Links `pico_cyw43_arch_lwip_sys_freertos` (threaded lwIP),
`FreeRTOS-Kernel`, and `FreeRTOS-Kernel-Heap4`.

```cmake
# Added before project()
set(FREERTOS_KERNEL_PATH ${USERHOME}/.pico-sdk/FreeRTOS-Kernel)
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2350_ARM_NTZ/FreeRTOS_Kernel_import.cmake)

# Changed link targets
pico_cyw43_arch_lwip_poll       →  pico_cyw43_arch_lwip_sys_freertos
+ FreeRTOS-Kernel
+ FreeRTOS-Kernel-Heap4

# Global compile definitions (before pico_sdk_init so SDK sources see them)
add_compile_definitions(CYW43_TASK_STACK_SIZE=2048 CYW43_TASK_PRIORITY=3)
```

### 2. FreeRTOS Configuration (`include/FreeRTOSConfig.h`)

RP2350 Cortex-M33 configuration:

| Setting | Value | Rationale |
|---------|-------|-----------|
| `configCPU_CLOCK_HZ` | 150 MHz | RP2350 default |
| `configTICK_RATE_HZ` | 1000 | 1ms tick |
| `configMAX_PRIORITIES` | 7 | See task table below |
| `configMINIMAL_STACK_SIZE` | 512 words | 2KB (increased from 256) |
| `configTOTAL_HEAP_SIZE` | 40 KB | FreeRTOS heap (heap_4.c, increased from 36KB) |
| `configUSE_RECURSIVE_MUTEXES` | 1 | Required by lwIP `sys_arch.c` |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | Both patterns checked |
| `configPRIO_BITS` | 4 | RP2350 NVIC priority bits |
| `configRUN_FREERTOS_SECURE_ONLY` | 1 | TrustZone disabled |

### 3. lwIP Configuration (`include/lwipopts.h`)

```c
// Before
#define NO_SYS  1   // bare-metal: no OS primitives

// After
#define NO_SYS  0   // threaded: lwIP runs in tcpip_thread
#define TCPIP_MBOX_SIZE       16    // default was 0 — caused assert
#define TCPIP_THREAD_STACKSIZE 2048 // default was too small
#define DEFAULT_THREAD_STACKSIZE 1024
```

### 4. BiDiB RX — FreeRTOS Stream Buffer (`bidib_client_if.c`)

**Before:** SPSC ring buffer with manual index management.
**After:** `StreamBufferHandle_t` with ISR-safe `xStreamBufferSendFromISR()`
and blocking `xStreamBufferReceive()`.

Created via `init_bidib_client_if_buffers()` — called before `bidib_init()`
so the stream buffer exists before PIO ISRs fire.

### 5. BiDiB TX — Hardware Spinlock (`bidib_client_if.c`, `bidib.c`)

**Before:** `bidib_enter_critical()` (disables interrupts on current core only).
**After:** `spin_lock_blocking(tx_spinlock)` (hardware spinlock, works across cores).

### 6. PIO ISR Priorities (`bidib.c`)

**Before:** Priority 0 (default) — too low for FreeRTOS API calls.
**After:** Priority 4 — must be ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`
(= 2, implied by `configPRIO_BITS=4`) to safely call `xStreamBufferSendFromISR()`.

### 7. Main Loop → FreeRTOS Tasks (`main.c`)

Task priorities and stack sizes:

| Task | Priority | Stack | Function |
|------|----------|-------|----------|
| BiDiB parser | 4 | 2048 words (8KB) | `run_bidib_client()` loop |
| CYW43 async (pico-sdk) | 3 | 2048 words (8KB) | WiFi driver event loop |
| Network init | 2 | 1536 words (6KB) | WiFi/TCP/roster/HTTP/smartphone init |
| Timer | 2 | 256 words | FreeRTOS internal |
| Log output | 1 | 512 words (2KB) | `log_poll()` UART drain |
| LED | 1 | 256 words | CYW43 GPIO blink control |
| Idle | 0 | 256 words | FreeRTOS internal |

BiDiB parser at prio 4 (highest) because the bus protocol is more
time-critical than WiFi. CYW43 overridden to prio 3 (default was 4).

Hooks:
- `vApplicationStackOverflowHook()` — prints task name, halts
- `vApplicationMallocFailedHook()` — prints message, halts
- `HardFault_Handler` / `isr_hardfault` — dumps CFSR/BFAR/PSP/PSPLIM

### 8. LED Control (`led.c`, `include/led.h`)

**New** dedicated LED task with state-driven control:

```c
typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,   // 500ms toggle
    LED_BLINK_FAST,   // 250ms toggle
} led_state_t;

void led_set_state(led_state_t state);   // any task, any time
void led_set_cyw43_ready(void);          // after cyw43_arch_init()
```

The LED task waits for `led_set_cyw43_ready()` before touching CYW43 GPIO,
since the async context lock is bound to the task that called
`cyw43_arch_init()` (network_task).

Flow: fast blink during boot → slow blink during WiFi connect → solid ON.

### 9. HTTP Server (`http_server.c`)

**Before:** `xml_start()` did multiple `tcp_write()` calls that could fail
with `ERR_MEM`, leaving the XML header incomplete. `xml_continue()` would
then skip the rest of the header.

**After:** Fully resumable phase state machine. Every chunk of output
(HTTP header, XML declaration, roster-config tags, per-loco data, closing
tags) gets its own phase number. If `tcp_write()` returns `ERR_MEM`, the
current chunk is retried on the next poll — no data is skipped.

### 10. Fault Handling (`main.c`)

Added PSPLIM register dump to both `HardFault_Handler` and `isr_hardfault`
overrides for Cortex-M33 stack overflow debugging:

```c
uint32_t psp, psplim;
__asm volatile ("mrs %0, psp" : "=r"(psp));
__asm volatile ("mrs %0, psplim" : "=r"(psplim));
printf("STKOF: PSP=0x%08lX PSPLIM=0x%08lX\n", psp, psplim);
```

---

## Task Architecture (After)

```
┌─────────────────────────────────────────────────────────┐
│                   FreeRTOS Scheduler                     │
├──────────────┬──────────────┬──────────────┬────────────┤
│ BiDiB Parser │ CYW43 async  │ Network Init │ Log Output │
│ prio 4       │ prio 3       │ prio 2       │ prio 1     │
│ 8KB stack    │ 8KB stack    │ 6KB stack    │ 2KB stack  │
│ run_bidib_   │ WiFi driver  │ WiFi/TCP/    │ log_poll() │
│ client()     │ events       │ roster/HTTP  │ → UART TX  │
├──────────────┴──────────────┼──────────────┤            │
│                             │   LED (1)    │            │
│              PIO0 ISRs      │ 256 words    │            │
│ RX: xStreamBufferSendFromISR│ CYW43 GPIO   │            │
│ TX: spin_lock_blocking      │ blink/on/off │            │
└─────────────────────────────┴──────────────┴────────────┘
```

---

## Testing Notes

- Build: `cmake -B build -G Ninja -DPICO_SDK_PATH=... -DPICO_BOARD=pico2_w && cmake --build build`
- Binary: 377 KB text, 96 KB bss (~473 KB total) after bss reduction
- On hardware: LED blinks during boot, WiFi connects, LED stays on.
  EngineDriver discovers via mDNS, BiDiB bus functions, HTTP roster
  serves on port 8080 without XML corruption.
- FreeRTOS scheduler runs all tasks; no `cyw43_arch_poll()` in main loop

---

# CHANGES — Flash-backed editable roster

## Overview

Replaced the 38 KB static in-RAM roster with a flash-backed roster stored
in the external W25Q32 SPI NOR (4 MB). Only a 4-slot LRU cache lives in
RAM. Adds an HTML edit UI on the existing HTTP server (port 8080) for
viewing, adding, editing, and deleting roster entries. All flash writes
go through a dedicated FreeRTOS task so the SPI stall (~200 ms per save)
never blocks lwIP, WiThrottle, or BiDiB.

**Binary impact:** 377 KB → 387 KB text (+10 KB), 96 KB → 103 KB bss (+7 KB).
Total ~490 KB — fits in Pico 2W's 520 KB SRAM.

**Flash usage:** 132 KB of external SPI flash (33 sectors × 4 KB) reserved
for header + 32 entry slots.

---

## Files Changed

| File | Change |
|------|--------|
| `include/roster.h` | New copy-out API, `ROSTER_MAX_ENTRIES=32`, `ROSTER_CACHE_SIZE=4` |
| `roster.c` | Rewrite: LRU cache, flash-backed storage, CRC32 header, bootstrap defaults |
| `include/roster_writer.h` | **New** — background flash writer interface |
| `roster_writer.c` | **New** — writer task (prio 1, 1 KB stack), queue, LED_ERROR on failure |
| `include/form_parser.h` | **New** — URL-encoded form parser |
| `form_parser.c` | **New** — `key=value&…`, `%XX`, `+` decoding, field table |
| `http_server.c` | Full rewrite: router, HTML routes, POST body accumulator, resumable generators |
| `include/flash_store.h` | Added `ROSTER_HEADER_ADDR`, `ROSTER_ENTRY_BASE`, `ROSTER_ENTRY_STRIDE`, magic/version constants |
| `include/led.h`, `led.c` | Added sticky `LED_ERROR` state + `led_clear_error()` |
| `withrottle_if.c` | Uses `roster_find_by_addr`, `roster_get_dense`; large locals moved to static/bss |
| `main.c` | Starts `roster_writer_init()` before scheduler |
| `CMakeLists.txt` | Added `roster_writer.c`, `form_parser.c`; optional `ROSTER_INJECT_FLASH_FAIL` debug flag |

---

## Storage layout (external W25Q32 flash)

```
0x000000  user string (24 B, existing)
0x001000  roster header sector — magic 'RSTR', version, used_bitmap, CRC32
0x002000  entry slot 0  (one 4 KB sector per entry)
0x003000  entry slot 1
   ...
0x021000  entry slot 31
```

Header struct (16 B):
```c
struct roster_header {
    uint32_t magic;         // 'RSTR' = 0x52535452
    uint16_t version;       // 1
    uint16_t reserved;
    uint32_t used_bitmap;   // bit N set = slot N in use
    uint32_t crc32;         // over the above 12 bytes
};
```

Blank/invalid header on first boot → `roster_bootstrap_defaults()` writes
the three original hardcoded entries (BR 185, BR 01, UP 4014) to flash
and installs a valid header.

## RAM data model

| Item | Size |
|---|---|
| 4-slot LRU cache of `roster_entry_t` | 4 × 1.2 KB = ~5 KB |
| Static scratch entry (mutex-guarded) | 1.2 KB |
| Shadow `used_bitmap` / `count` | 8 B |
| HTTP POST body buffer | 3 KB |
| HTTP `g_gen_buf` (shared HTML/XML generator scratch) | 768 B |
| Writer task stack | 1 KB |
| Writer queue (2 × job) | 2.4 KB |

## Public API (roster.h)

All accessors are thread-safe (recursive mutex) and copy the entry into
a caller-supplied buffer — pointers into the cache are never returned.

```c
bool roster_init(void);
bool roster_get(uint8_t slot, roster_entry_t *out);
bool roster_get_dense(uint8_t dense_idx, uint8_t *out_slot, roster_entry_t *out);
bool roster_find_by_addr(uint16_t dcc, roster_entry_t *out);
bool roster_find_by_id(const char *id, uint8_t *out_slot);
bool roster_add(const roster_entry_t *in, uint8_t *out_slot);   // auto-picks lowest free slot; rejects duplicate id
bool roster_update(uint8_t slot, const roster_entry_t *in);     // rejects duplicate id in another slot
bool roster_delete(uint8_t slot);
uint8_t roster_count_valid(void);
```

## Writer task & concurrency

- **tcpip_thread** calls `roster_*` mutations → updates cache under mutex →
  enqueues a `roster_write_job_t` on `roster_write_queue` → HTTP 302 goes
  out immediately.
- **roster_writer_task** (prio 1, 1 KB stack) dequeues jobs and calls
  `flash_store_write` (~200 ms per sector). Only this task touches the
  flash for roster data.
- **On flash-write failure:** `LOG_ERROR` → `roster_rollback_from_flash(slot)`
  re-reads the affected slot + header so RAM matches what actually
  persisted → `led_set_state(LED_ERROR)` sticky fast-blink until reboot.

Mutex is **never** held across `flash_store_write`.

## HTTP routes

| Method | Path | Response |
|---|---|---|
| GET | `/` | 302 → `/roster.html` |
| GET | `/roster.html` | HTML table + [Add new], [Edit], [Delete] controls |
| GET | `/roster/` | JMRI XML (unchanged — EngineDriver still works) |
| GET | `/edit?slot=N` or `?slot=new` | HTML form pre-filled from slot N, or blank |
| POST | `/save` | Parse form body, validate, `roster_add`/`update`, 302 |
| POST | `/delete?slot=N` | `roster_delete`, 302 |
| any other | | 404 |

Chosen framework: **none.** Extended the existing raw-lwIP HTTP server.
Adding mongoose / lwIP-httpd would have cost ≥ 20 KB flash for 5 routes.
All HTML skeleton is `const char[]` in `.rodata` (flash-resident, 0 RAM).
Dynamic parts stream through `g_gen_buf` (single shared 768 B buffer).
All response generators are resumable — if `tcp_write` returns `ERR_MEM`,
the current chunk is retried on the next `tcp_poll` without skipping.

## Form parser

Parses `application/x-www-form-urlencoded` bodies. URL-decoded key/value
pairs are matched against a field table (`offsetof` + type tag) and
written directly into a caller `roster_entry_t`. Function fields
(`fN_label`, `fN_lockable`, `fN_visible`) are handled by a prefix parser.
No dynamic allocation.

## Validation on POST /save

- `id` must not be empty
- `dccAddress` in [1, 10239]
- `maxFnNum` ≤ `ROSTER_FUNC_MAX` (28)
- **Duplicate `id` rejected** (in `roster_add` and cross-slot in
  `roster_update`) → 400 with error page

## LED behavior

| State | Meaning |
|---|---|
| `LED_BLINK_FAST` | boot / init in progress |
| `LED_BLINK_SLOW` | WiFi connecting |
| `LED_ON` | ready |
| `LED_OFF` | WiFi failed |
| `LED_ERROR` | **sticky** — flash write failed, roster rolled back from flash |

`LED_ERROR` can only be cleared by `led_clear_error()` (or reboot).

## Debug: flash-fail injection

Rebuild with `-DROSTER_INJECT_FLASH_FAIL=N` (CMake configure) to force
every Nth `flash_store_write` in the writer task to fail. Exercises the
rollback + `LED_ERROR` path without needing hardware to actually fail.

Verified on hardware (2026-09-13):
- Bootstrap defaults on blank flash: OK
- GET `/roster.html`: OK
- GET `/edit?slot=0`: form pre-fills correctly
- POST `/save`: cache updates, background flash write (`wrote 1216 bytes @ 0x002000` + `16 bytes @ 0x001000`)
- POST `/save` with `-DROSTER_INJECT_FLASH_FAIL=1`: `flash_store_write failed`, rollback + `LED_ERROR` fires
- EngineDriver WiThrottle continues to work throughout

## Stack-overflow fixes required for this migration

Large `roster_entry_t` locals (~1.2 KB) on the tcpip_thread stack triggered
Cortex-M33 STKOF during WiThrottle connect and HTTP save. Fixes:
- `roster.c`: shared static `scratch` (mutex-guarded) replaces per-call `tmp`
- `withrottle_if.c` `send_welcome_message`: `entry` moved to `static`
- `http_server.c` `handle_save`: reuses `http_conn.entry` (bss) instead of stack
- HTML/XML generator scratch buffers (128/512/768 B) consolidated into
  one static `g_gen_buf[768]` — saves ~1.3 KB of stack per generator call

## HTTP bug fix

`tcp_recved()` was being called on a pcb that `dispatch()` had already
closed via `tcp_close()` (use-after-close). Moved `tcp_recved()` before
`dispatch()`. Suppressed spurious `ERR_ABRT` / `ERR_RST` / `ERR_CLSD`
warnings from `err_cb` — those are normal peer-side close scenarios.
