/*
 * log.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Ring buffer non-bloquant pour les logs.
 *   - log_printf()  : formate et pousse dans le ring buffer (aucun I/O)
 *   - log_poll()    : à appeler dans la boucle principale, draine le buffer
 *                     vers l'UART (bridge du debug probe) sans bloquer.
 *
 * L'UART est initialisé par stdio_init_all() (stdio_uart). Si le FIFO TX
 * est plein, les octets restants sont simplement conservés pour l'appel
 * suivant de log_poll() — jamais de blocage.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/irq.h"
#include "hardware/uart.h"
#include "FreeRTOS.h"
#include "task.h"

#include "log.h"

static uint8_t  log_ring[LOG_RING_SIZE];
static volatile uint32_t log_head = 0;   // prochaine écriture
static volatile uint32_t log_tail = 0;   // prochaine lecture

static uint32_t log_ring_used(void) {
    return (log_head - log_tail + LOG_RING_SIZE) % LOG_RING_SIZE;
}

void log_init(void) {
    log_head = 0;
    log_tail = 0;
}

static void log_push(const char *data, int n) {
    UBaseType_t ints;
    if (xPortIsInsideInterrupt()) {
        ints = taskENTER_CRITICAL_FROM_ISR();
    } else {
        taskENTER_CRITICAL();
    }
    for (int i = 0; i < n; i++) {
        log_ring[log_head] = (uint8_t)data[i];
        log_head = (log_head + 1) % LOG_RING_SIZE;
        if (log_head == log_tail) {
            log_tail = (log_tail + 1) % LOG_RING_SIZE;  // overflow -> drop
        }
    }
    if (xPortIsInsideInterrupt()) {
        taskEXIT_CRITICAL_FROM_ISR(ints);
    } else {
        taskEXIT_CRITICAL();
    }
}

void log_printf(const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    log_push(tmp, n);
}

// Like snprintf() but also pushes the resulting string into the ring buffer
// (useful for tracing TCP responses while filling the send buffer).
int log_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n > 0) {
        int log_n = n;
        if (size > 0 && log_n >= (int)size) log_n = (int)size - 1;  // tronqué
        log_push(buf, log_n);
    }
    return n;   // même sémantique que snprintf
}

void log_poll(void) {
    if (log_head == log_tail) return;

    uart_inst_t *uart = uart_get_instance(PICO_DEFAULT_UART);
    uint32_t sent_total = 0;

    while (log_head != log_tail) {
        if (!uart_is_writable(uart)) break;  // FIFO TX full -> non-blocking

        UBaseType_t ints;
        if (xPortIsInsideInterrupt()) {
            ints = taskENTER_CRITICAL_FROM_ISR();
        } else {
            taskENTER_CRITICAL();
        }
        uint8_t b = log_ring[log_tail];
        log_tail = (log_tail + 1) % LOG_RING_SIZE;
        if (xPortIsInsideInterrupt()) {
            taskEXIT_CRITICAL_FROM_ISR(ints);
        } else {
            taskEXIT_CRITICAL();
        }

        uart_putc_raw(uart, b);
        if (++sent_total >= 512) break;  // budget per call
    }
}
