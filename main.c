/*
 * main.c — BIDIBrc Pico 2W
 * Point d'entrée principal
 *
 * v0.2 : ajout WiFi AP + TCP WiThrottle
 * Le bus BiDiB (PIO) et le WiFi tournent en parallèle dans la même boucle :
 *   - cyw43_arch_poll() gère les callbacks TCP/lwIP
 *   - Les ISR PIO (bidib_pio_rx_isr / tx_isr) sont déclenchées par hardware
 *     indépendamment de la boucle → pas d'interférence
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"

#include "bidib.h"
#include "tcp_server.h"
#include "smartphone_if.h"
#include "bidib_client_parser.h"
#include "config.h"

static const char *TAG = "main";

int main(void)
{
    stdio_init_all();
    sleep_ms(3000);  // attendre USB serial
    LOG_INFO(TAG,"=== WI_BIDIB_ED Pico 2W v0.2 ===");
    stdio_flush();

    // ── BiDiB PIO (inchangé) ──────────────────────────────────────────────────
    // ISR RX/TX enregistrées dans bidib_init(), tournent en hardware
    bidib_init();
    LOG_INFO(TAG,"BiDiB PIO OK");
    init_bidib_client();
    LOG_INFO(TAG,"BiDiB client init OK");

    // ── WiFi AP + TCP WiThrottle ──────────────────────────────────────────────
    smartphone_if_init();   // init table throttle[] + UID BiDiB

    if (!wifi_init_softap()) {
        printf("WiFi AP ERREUR — on continue sans WiFi\n");
        // On ne bloque pas : le BiDiB seul reste fonctionnel
    } else {
        if (!tcp_server_init()) {
            printf("TCP server ERREUR\n");
        } else {
            LOG_INFO(TAG,"WiFi AP + TCP OK — SSID:myssid port:5550");
        }
    }

    printf("Boucle principale\n");


    // ── Boucle principale ─────────────────────────────────────────────────────
    //
    // cyw43_arch_poll() déclenche les callbacks TCP (recv, accept, err)
    //   → process_rx_withrottle() appelé à l'intérieur
    //
    // Les ISR BiDiB PIO tournent indépendamment (hardware IRQ)
    //   → bidib_pio_rx_isr() / bidib_pio_tx_isr() non affectés par le poll
    //
    while (1) {
        cyw43_arch_poll();  // traite WiFi + lwIP callbacks
        run_bidib_client(); 
      //  sleep_ms(1);
    }

    cyw43_arch_deinit();
    return 0;
}