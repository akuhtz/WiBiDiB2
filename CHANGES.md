# CHANGES — FreeRTOS Migration

## Overview

Migrated WiBiDiB2 from bare-metal polling (`NO_SYS=1`) to FreeRTOS
(`NO_SYS=0`) on the RP2350 Cortex-M33. This converts lwIP from raw-poll
mode to threaded mode, runs the BiDiB parser and log output as dedicated
FreeRTOS tasks, and adds dual-core safety via hardware spinlocks.

**Branch:** `freertos`
**Binary impact:** 362KB → 377KB text (+15KB), 92KB → 140KB bss (+48KB).
Total 517KB — fits in Pico 2W's 520KB SRAM.

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
- Binary: 377KB text, 140KB bss (517KB total)
- On hardware: LED blinks during boot, WiFi connects, LED stays on.
  EngineDriver discovers via mDNS, BiDiB bus functions, HTTP roster
  serves on port 8080 without XML corruption.
- FreeRTOS scheduler runs all tasks; no `cyw43_arch_poll()` in main loop
