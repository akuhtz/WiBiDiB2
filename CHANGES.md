# CHANGES — FreeRTOS Migration

## Overview

Migrated WiBiDiB2 from bare-metal polling (`NO_SYS=1`) to FreeRTOS
(`NO_SYS=0`) on the RP2350 Cortex-M33. This converts lwIP from raw-poll
mode to threaded mode, runs the BiDiB parser and log output as dedicated
FreeRTOS tasks, and adds dual-core safety via hardware spinlocks.

**Branch:** `freertos` (2 commits on top of `main`)
**Binary impact:** 362KB → 380KB text (+18KB), 92KB → 132KB bss (+40KB).
Total 511KB — fits in Pico 2W's 520KB SRAM.

---

## Files Changed

| File | Change |
|------|--------|
| `CMakeLists.txt` | FreeRTOS kernel import, new link targets |
| `include/FreeRTOSConfig.h` | **New file** — RP2350 kernel configuration |
| `include/lwipopts_examples_common.h` | `NO_SYS` 1 → 0 |
| `include/bidib.h` | Critical sections → FreeRTOS-aware |
| `include/bidib_client_if.h` | RX buffer externs removed, spinlock extern added |
| `bidib_client_if.c` | RX stream buffer, TX spinlock |
| `bidib.c` | TX ISR + parser TX use spinlock |
| `log.c` | Critical sections → FreeRTOS-aware |
| `main.c` | Tasks + scheduler replaces `while(1)` loop |
| `tcp_server.c` | `cyw43_arch_poll()` → `vTaskDelay()` |

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

# Added compile definitions
FREERTOS_CONFIG_FILE_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/include"
FREERTOS_KERNEL_INCLUDE_DIR="${FREERTOS_KERNEL_PATH}/include"
CYW43_TASK_PRIORITY=3            # WiFi below BiDiB parser (prio 4)
```

### 2. FreeRTOS Configuration (`include/FreeRTOSConfig.h`) — NEW

RP2350 Cortex-M33 configuration:

| Setting | Value | Rationale |
|---------|-------|-----------|
| `configCPU_CLOCK_HZ` | 150 MHz | RP2350 default |
| `configTICK_RATE_HZ` | 1000 | 1ms tick |
| `configMAX_PRIORITIES` | 5 | WiFi(4), BiDiB(3), Timer(2), Log(1), Idle(0) |
| `configMINIMAL_STACK_SIZE` | 256 words | 1KB |
| `configTOTAL_HEAP_SIZE` | 32 KB | FreeRTOS heap (heap_4.c) |
| `configUSE_RECURSIVE_MUTEXES` | 1 | Required by lwIP `sys_arch.c` |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | Both patterns checked |
| `configPRIO_BITS` | 4 | RP2350 NVIC priority bits |
| `configRUN_FREERTOS_SECURE_ONLY` | 1 | TrustZone disabled |

Hooks mapped to Cortex-M33 vectors:
```c
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler
#define vPortSVCHandler       SVC_Handler
```

### 3. lwIP Configuration (`include/lwipopts_examples_common.h`)

```c
// Before
#define NO_SYS  1   // bare-metal: no OS primitives

// After
#define NO_SYS  0   // threaded: lwIP runs in tcpip_thread
```

This switches lwIP from raw callback API to sequential API. All TCP/UDP
operations now go through `tcpip_thread` — safer and required for
CYW43 WiFi driver integration with FreeRTOS.

### 4. Critical Sections (`include/bidib.h`)

**Before:** `save_and_disable_interrupts()` / `restore_interrupts()` (Pico SDK).
**After:** FreeRTOS-aware critical sections that detect ISR vs task context:

```c
static inline uint32_t bidib_enter_critical(void) {
    if (xPortIsInsideInterrupt()) {
        return taskENTER_CRITICAL_FROM_ISR();
    } else {
        taskENTER_CRITICAL();
        return 0;
    }
}

static inline void bidib_exit_critical(uint32_t state) {
    if (xPortIsInsideInterrupt()) {
        taskEXIT_CRITICAL_FROM_ISR(state);
    } else {
        taskEXIT_CRITICAL();
    }
}
```

Used by `log.c` and other code that may be called from both ISR and task
contexts. The BiDiB TX path now uses hardware spinlocks instead (see below).

### 5. BiDiB RX — FreeRTOS Stream Buffer (`bidib_client_if.c`)

**Before:** SPSC (single-producer, single-consumer) ring buffer:
```c
uint16_t bidib_rx_buf[BIDIB_RX_BUF_SIZE];  // 64 entries
uint8_t  bidib_rx_buf_read, bidib_rx_buf_write;
```

**After:** FreeRTOS stream buffer:
```c
#define BIDIB_RX_STREAM_SIZE  256  // bytes (128 x uint16_t)
#define BIDIB_RX_TRIGGER      2    // wake on 1 word
static StreamBufferHandle_t bidib_rx_stream;
```

| Operation | Before | After |
|-----------|--------|-------|
| ISR write | Direct array write + index advance | `xStreamBufferSendFromISR()` |
| Task read | Direct array read + index advance | `xStreamBufferReceive()` |
| Check ready | `read != write` | `xStreamBufferBytesAvailable() >= 2` |
| Flush | Manual index reset | `xStreamBufferReset()` |

Benefits:
- ISR-safe without disabling interrupts
- Consumer task blocks until data available (no busy-wait)
- Clean API boundary between ISR and task

### 6. BiDiB TX — Hardware Spinlock (`bidib_client_if.c`, `bidib.c`)

**Before:** `bidib_enter_critical()` (disables interrupts on current core only).
**After:** `spin_lock_blocking(tx_spinlock)` (hardware spinlock, works across cores).

```c
spin_lock_t *tx_spinlock;  // initialized in init_bidib_client_if()

// Producer (task context): bidib_tx_fifo_put()
uint32_t saved = spin_lock_blocking(tx_spinlock);
// ... write to bidib_tx_buf, advance bidib_tx_buf_write, bidib_tx_ahead ...
spin_unlock(tx_spinlock, saved);

// Consumer (ISR context): bidib_pio_tx_isr()
uint32_t saved = spin_lock_blocking(tx_spinlock);
// ... read from bidib_tx_buf, advance bidib_tx_buf_read, bidib_tx_fill ...
spin_unlock(tx_spinlock, saved);
```

Protected variables: `bidib_tx_buf[]`, `bidib_tx_buf_read`,
`bidib_tx_buf_write`, `bidib_tx_fill`, `bidib_tx_ahead`.

Protected code paths:
- `bidib_tx_fifo_put()` — task writes message to TX buffer
- `bidib_start_parser_tx()` — task starts sending next message
- `bidib_pio_tx_isr()` — ISR chains next message after CRC sent
- `bidib_pio_rx_isr()` — ISR responds to poll with pending data
- `bidib_tx_fifo_empty/ready/okay/healthy()` — task checks buffer state
- `bidib_flush_tx()` — task resets buffer

Benefits:
- Correct on dual-core RP2350 (task and ISR may run on different cores)
- Spinlock held for very short durations only
- `next_striped_spin_lock_num()` distributes spinlocks to reduce contention

### 7. Log Critical Sections (`log.c`)

**Before:** `save_and_disable_interrupts()` / `restore_interrupts()`.
**After:** FreeRTOS-aware `taskENTER_CRITICAL_FROM_ISR()` / `taskEXIT_CRITICAL_FROM_ISR()`.

Applied to both `log_push()` (called from any context) and `log_poll()`
(called from log output task). Detects ISR context via `xPortIsInsideInterrupt()`.

### 8. Main Loop → FreeRTOS Tasks (`main.c`)

**Before:**
```c
while (1) {
    cyw43_arch_poll();
    log_poll();
    run_bidib_client();
}
```

**After:**
```c
// WiFi/LWIP — handled by pico-sdk CYW43 async context (prio 3)
// BiDiB parser task
xTaskCreate(bidib_parser_task, "bidib_parser", 512, NULL, 4,
            &bidib_parser_task_handle);
// Log output task
xTaskCreate(log_output_task, "log_output", 256, NULL, 1,
            &log_task_handle);

vTaskStartScheduler();  // never returns
```

Task priorities (BiDiB highest — real-time bus protocol):
| Task | Priority | Stack | Function |
|------|----------|-------|----------|
| BiDiB parser | 4 | 2KB | `run_bidib_client()` loop |
| WiFi/LWIP (pico-sdk) | 3 | managed by SDK | CYW43 async context |
| Timer | 2 | 1KB | FreeRTOS internal |
| Log output | 1 | 1KB | `log_poll()` UART drain |
| Idle | 0 | 256 words | FreeRTOS internal |

WiFi priority overridden via `CYW43_TASK_PRIORITY=3` in CMakeLists.txt
(default is 4). BiDiB parser runs at priority 4 to ensure timely token
and poll handling — the bus protocol cannot tolerate delays.

Added hooks:
- `vApplicationStackOverflowHook()` — prints task name, halts
- `vApplicationMallocFailedHook()` — prints message, halts

### 9. TCP Server (`tcp_server.c`)

**Before:** `cyw43_arch_poll()` + `sleep_ms(100)` in STA connection wait loop.
**After:** `vTaskDelay(pdMS_TO_TICKS(100))` — yields to other tasks while waiting.

The `cyw43_arch_poll()` call is no longer needed; the CYW43 driver runs
in its own async context managed by the pico-sdk FreeRTOS integration.

---

## Task Architecture (After)

```
┌─────────────────────────────────────────────────┐
│                 FreeRTOS Scheduler               │
├──────────────┬──────────────┬───────────────────┤
│ BiDiB Parser │ WiFi/LWIP    │ Log Output        │
│ prio 4       │ prio 3       │ prio 1            │
│ run_bidib_   │ CYW43 async  │ log_poll()        │
│ client()     │ context      │ → UART TX         │
│ → TX/RX msgs │ (pico-sdk)   │                   │
├──────────────┴──────────────┴───────────────────┤
│              PIO0 ISRs (hardware)                │
│ RX: bidib_pio_rx_isr()  TX: bidib_pio_tx_isr() │
│ → xStreamBufferSendFromISR()  → spin_lock       │
└─────────────────────────────────────────────────┘
```

---

## Testing Notes

- Build: `cmake -B build -G Ninja -DPICO_SDK_PATH=... -DPICO_BOARD=pico2_w && cmake --build build`
- Binary: 380KB text, 132KB bss (511KB total)
- On hardware: WiFi AP starts, EngineDriver discovers via mDNS, BiDiB bus
  functions, HTTP roster serves on port 8080
- FreeRTOS scheduler runs all tasks; no `cyw43_arch_poll()` in main loop
