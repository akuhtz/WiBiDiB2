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

int main(void)
{
    stdio_init_all();
    log_init();
    sleep_ms(3000);
    LOG_INFO(TAG, "=== WiBiDiB2 Pico 2W (FreeRTOS) ===");
    stdio_flush();

    // ── Flash (must precede init_bidib_client for user string) ──────────
    if (!flash_store_init()) {
        LOG_WARN(TAG, "flash absent -- continuing without storage");
    }

    // ── BiDiB PIO ──────────────────────────────────────────────────────
    bidib_init();
    LOG_INFO(TAG, "BiDiB PIO OK");
    init_bidib_client();
    LOG_INFO(TAG, "BiDiB client init OK");

    // ── WiFi + TCP ─────────────────────────────────────────────────────
    if (!wifi_init()) {
        LOG_WARN(TAG, "WiFi failed -- continuing without WiFi");
    } else {
        if (!tcp_server_init()) {
            LOG_ERROR(TAG, "TCP server init failed");
        } else {
            LOG_INFO(TAG, "WiFi + TCP OK -- port: %d", WITHROTTLE_PORT);
        }
    }

    // ── Roster + HTTP server ───────────────────────────────────────────
    roster_init();
    LOG_INFO(TAG, "Roster: %d entries", roster.count);

    if (!http_server_init()) {
        LOG_WARN(TAG, "HTTP server init failed");
    }

    // ── Smartphone interface ───────────────────────────────────────────
    smartphone_if_init();

    // ── Create FreeRTOS tasks ──────────────────────────────────────────
    xTaskCreate(bidib_parser_task, "bidib_parser", 512, NULL, 3, &bidib_parser_task_handle);
    xTaskCreate(log_output_task,   "log_output",   256, NULL, 1, &log_task_handle);

    LOG_INFO(TAG, "Starting FreeRTOS scheduler");
    vTaskStartScheduler();

    // Should never reach here
    configASSERT(0);
    for (;;) {}
}