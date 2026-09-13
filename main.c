/*
 * main.c — BIDIBrc Pico 2W
 * Point d'entrée principal
 *
 * FreeRTOS architecture:
 *   - Task 1 (prio 4): WiFi/LWIP — handled by pico-sdk (CYW43 async context)
 *   - Task 2 (prio 3): BiDiB parser — run_bidib_client()
 *   - Task 3 (prio 1): Log output — log_poll()
 *   - ISRs: PIO0_IRQ_0 (RX), PIO0_IRQ_1 (TX) — highest priority, untouchable
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"
#include "FreeRTOS.h"
#include "task.h"

#include "bidib.h"
#include "tcp_server.h"
#include "smartphone_if.h"
#include "bidib_client_parser.h"
#include "flash_store.h"
#include "http_server.h"
#include "roster.h"
#include "roster_writer.h"
#include "led.h"
#include "config.h"

static const char *TAG = "main";

// ─── FreeRTOS hooks ──────────────────────────────────────────────────────
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    printf("STACK OVERFLOW in task: %s\n", pcTaskName);
    configASSERT(0);
}

void vApplicationMallocFailedHook(void) {
    printf("MALLOC FAILED\n");
    configASSERT(0);
}

void HardFault_Handler(void) {
    uint32_t cfsr = *((volatile uint32_t*)0xE000ED28);
    uint32_t bfar = *((volatile uint32_t*)0xE000ED38);
    uint32_t mmfar = *((volatile uint32_t*)0xE000ED34);
    uint32_t lr;
    __asm volatile ("mov %0, lr" : "=r"(lr));
    printf("HARDFAULT: CFSR=0x%08lX BFAR=0x%08lX MMFAR=0x%08lX LR=0x%08lX\n", cfsr, bfar, mmfar, lr);
    if (cfsr & (1UL << 20)) {
        uint32_t psp, psplim;
        __asm volatile ("mrs %0, psp" : "=r"(psp));
        __asm volatile ("mrs %0, psplim" : "=r"(psplim));
        printf("STKOF: PSP=0x%08lX PSPLIM=0x%08lX (used=%ld)\n",
               psp, psplim, (long)(psplim - psp));
    }
    for (;;) {}
}

// Override SDK's weak isr_hardfault (used in vector table)
void isr_hardfault(void) {
    uint32_t cfsr = *((volatile uint32_t*)0xE000ED28);
    uint32_t bfar = *((volatile uint32_t*)0xE000ED38);
    uint32_t mmfar = *((volatile uint32_t*)0xE000ED34);
    uint32_t lr;
    __asm volatile ("mov %0, lr" : "=r"(lr));
    printf("HARDFAULT: CFSR=0x%08lX BFAR=0x%08lX MMFAR=0x%08lX LR=0x%08lX\n", cfsr, bfar, mmfar, lr);
    if (cfsr & (1UL << 20)) {
        uint32_t psp, psplim;
        __asm volatile ("mrs %0, psp" : "=r"(psp));
        __asm volatile ("mrs %0, psplim" : "=r"(psplim));
        printf("STKOF: PSP=0x%08lX PSPLIM=0x%08lX\n", psp, psplim);
    }
    for (;;) {}
}

// ─── Task handles ────────────────────────────────────────────────────────
static TaskHandle_t bidib_parser_task_handle;
static TaskHandle_t log_task_handle;

// ─── BiDiB parser task ──────────────────────────────────────────────────
static void bidib_parser_task(void *param) {
    (void)param;
    LOG_INFO(TAG, "BiDiB parser task started");
    for (;;) {
        run_bidib_client();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ─── Log output task ────────────────────────────────────────────────────
static void log_output_task(void *param) {
    (void)param;
    LOG_INFO(TAG, "Log output task started");
    for (;;) {
        log_poll();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── WiFi + network init task ────────────────────────────────────
// Runs cyw43_arch_init() in a proper task context so the async
// context lock is bound to this task (not the dead main task).
static void network_task(void *param) {
    (void)param;

    led_set_state(LED_BLINK_SLOW);

    if (!wifi_init()) {
        LOG_WARN(TAG, "WiFi failed -- continuing without WiFi");
        led_set_state(LED_OFF);
    } else {
        led_set_state(LED_ON);
        if (!tcp_server_init()) {
            LOG_ERROR(TAG, "TCP server init failed");
        } else {
            LOG_INFO(TAG, "WiFi + TCP OK -- port: %d", WITHROTTLE_PORT);
        }
    }

    roster_init();
    LOG_INFO(TAG, "Roster: %d entries", roster_count_valid());

    if (!http_server_init()) {
        LOG_WARN(TAG, "HTTP server init failed");
    }

    // printf("DEBUG: before smartphone_if_init\n");
    smartphone_if_init();
    // printf("DEBUG: after smartphone_if_init\n");    
    // smartphone_if_init();
    log_poll();

    // Keep task alive — CYW43 async context needs this owner
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main(void)
{
    stdio_init_all();
    log_init();
    sleep_ms(3000);
    LOG_INFO(TAG, "=== WiBiDiB2 Pico 2W (FreeRTOS) ===");
    stdio_flush();

    // ── LED task — runs immediately, waits for CYW43 before touching GPIO ──
    led_task_init();
    led_set_state(LED_BLINK_FAST);

    // ── Flash (must precede init_bidib_client for user string) ──────────
    if (!flash_store_init()) {
        LOG_WARN(TAG, "flash absent -- continuing without storage");
    }

    // ── BiDiB PIO ──────────────────────────────────────────────────────
    init_bidib_client_if_buffers();  // stream buffer + spinlock (before PIO ISRs)
    bidib_init();
    LOG_INFO(TAG, "BiDiB PIO OK");
    init_bidib_client();
    LOG_INFO(TAG, "BiDiB client init OK");

    // ── Create FreeRTOS tasks ──────────────────────────────────────────
    // Network task (prio 2) — owns CYW43 async context
    xTaskCreate(network_task, "network", 1536, NULL, 2, NULL);
    // BiDiB parser task (prio 4) — highest, real-time bus
    xTaskCreate(bidib_parser_task, "bidib_parser", 2048, NULL, 4, &bidib_parser_task_handle);
    // Log output task (prio 1) — UART drain
    xTaskCreate(log_output_task,   "log_output",   512, NULL, 1, &log_task_handle);
    // Roster writer task (prio 1) — background flash writes
    if (!roster_writer_init()) {
        LOG_WARN(TAG, "roster_writer_init failed");
    }

    // Drain any LOG messages accumulated during init (before tasks run)
    log_poll();

    LOG_INFO(TAG, "Starting FreeRTOS scheduler");
    log_poll();

    vTaskStartScheduler();

    // Should never reach here
    configASSERT(0);
    for (;;) {}
}