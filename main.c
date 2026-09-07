/*
 * main.c — BIDIBrc Pico 2W
 * Point d'entrée principal
 *
 * v0.2 : ajout WiFi AP + TCP WiThrottle
 * Le bus BiDiB (PIO) et le WiFi tournent en parallèle dans la même boucle :
 *   - cyw43_arch_poll() gère les callbacks TCP/lwIP
 *   - Les ISR PIO (bidib_pio_rx_isr / tx_isr) sont déclenchées par hardware
 *     indépendamment de la boucle → not d'interférence
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"

#include "bidib.h"
#include "tcp_server.h"
#include "smartphone_if.h"
#include "bidib_client_parser.h"
#include "flash_store.h"
#include "http_server.h"
#include "roster.h"
#include "config.h"

static const char *TAG = "main";

int main(void)
{
    stdio_init_all();
    log_init();        // ring buffer de log non-bloquant
    sleep_ms(3000);  // attendre USB serial
    LOG_INFO(TAG,"=== WI_BIDIB_ED Pico 2W v0.2 ===");
    stdio_flush();

    // // ── BiDiB PIO (unchanged) ──────────────────────────────────────────────────
    // // RX/TX ISRs registered in bidib_init(), run in hardware
    // bidib_init();
    // LOG_INFO(TAG,"BiDiB PIO OK");
    // init_bidib_client();
    // LOG_INFO(TAG,"BiDiB client init OK");

    // // ── WiFi AP + TCP WiThrottle ──────────────────────────────────────────────
    // smartphone_if_init();   // init table throttle[] + UID BiDiB

    if (!wifi_init()) {
        printf("WiFi ERREUR — on continue sans WiFi\n");
        // On ne bloque not : le BiDiB seul reste fonctionnel
    } else {
        if (!tcp_server_init()) {
            printf("TCP server ERREUR\n");
        } else {
            LOG_INFO(TAG,"WiFi + TCP OK — port:5550");
        }
    }

    // ── Roster + HTTP server ─────────────────────────────────────────────
    roster_init();
    LOG_INFO(TAG,"Roster: %d entries", roster.count);

    if (!http_server_init()) {
        LOG_WARN(TAG, "HTTP server init failed");
    }

    printf("Boucle principale\n");

    // ── Flash externe W25Q32VFSIG (SPI1) ────────────────────────────────────
    // Non-blocking for BiDiB: PIO ISRs (priority 0) run during
    // les transferts SPI. En cas d'absence du circuit, on continue.
    // Must precede init_bidib_client() (loads the user string).
    if (!flash_store_init()) {
        LOG_WARN(TAG, "flash externe absente — on continue sans stockage");
    }

    // ── BiDiB PIO (unchanged) ──────────────────────────────────────────────────
    // RX/TX ISRs registered in bidib_init(), run in hardware
    bidib_init();
    LOG_INFO(TAG,"BiDiB PIO OK");
    init_bidib_client();
    LOG_INFO(TAG,"BiDiB client init OK");

    // ── WiFi AP + TCP WiThrottle ──────────────────────────────────────────────
    smartphone_if_init();   // init table throttle[] + UID BiDiB

    // ── Boucle principale ─────────────────────────────────────────────────────
    //
    // cyw43_arch_poll() triggers TCP callbacks (recv, accept, err)
    //   → process_rx_withrottle() called inside
    //
    // BiDiB PIO ISRs run independently (hardware IRQ)
    //   → bidib_pio_rx_isr() / bidib_pio_tx_isr() not affected by poll
    //
    while (1) {
        cyw43_arch_poll();  // traite WiFi + lwIP callbacks
        log_poll();         // draine le ring buffer vers USB sans bloquer
        run_bidib_client(); 
      //  sleep_ms(1);
    }

    cyw43_arch_deinit();
    return 0;
}