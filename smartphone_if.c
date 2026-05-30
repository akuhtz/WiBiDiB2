/*
 * smartphone_if.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Portage depuis ESP32
 * Contient : table throttle[], cli_index, helpers find_*
 * La création d'UID BiDiB et le pairing BiDiB sur TCP (commenté sur ESP32)
 * restent commentés — prévu pour phase suivante avec RN485.
 *
 * Pierre Moulin — portage 2025
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/unique_id.h"   // pour créer l'UID BiDiB à partir du flash ID

#include "config.h"
#include "datatypes.h"
#include "withrottle_if.h"

static const char *TAG = "smart_if";

// ─── Tables globales ──────────────────────────────────────────────────────────
throttle_t throttle[MAX_THROTTLES];
uint8_t    cli_index = 0;

// ─── Helpers table throttle[] ────────────────────────────────────────────────

// Remplace is_sock_in_throttleTab(sock) ESP32
uint8_t find_throttle_by_pcb(struct tcp_pcb *pcb) {
    for (uint8_t j = 0; j < MAX_THROTTLES; j++) {
        if (throttle[j].pcb == pcb) return j;
    }
    return 255;  // non trouvé
}

uint8_t find_free_throttle_slot(void) {
    for (uint8_t j = 0; j < MAX_THROTTLES; j++) {
        if (throttle[j].pcb == NULL) return j;
    }
    return 255;  // table pleine
}

// ─── Initialisation ──────────────────────────────────────────────────────────
void smartphone_if_init(void) {
    memset(throttle, 0, sizeof(throttle));
    cli_index = 0;
    LOG_INFO(TAG, "smartphone_if initialized, max %d clients", MAX_THROTTLES);
}
