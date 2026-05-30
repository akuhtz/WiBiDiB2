/*
 * bidib_client_parser.h  —  WiBiDiB (Pico 2W)
 * Pierre Moulin 2025
 */

#ifndef BIDIB_CLIENT_PARSER_H_
#define BIDIB_CLIENT_PARSER_H_

#include <stdint.h>
#include <stdbool.h>
#include "bidib_messages.h"
#include "bidib_client_if.h"

// ─── État connexion ───────────────────────────────────────────────────────────
extern uint8_t g_bidib_connect;
extern uint8_t my_bidib_node_addr;
extern uint8_t g_bidib_backoff;

// ─── API publique ─────────────────────────────────────────────────────────────
void init_bidib_client(void);
void run_bidib_client(void);
void set_bidib_state(uint8_t neu, uint8_t assigned_addr);

bool bidib_send_onepara_msg(uint8_t msg_type, uint8_t dat);
bool send_bidib_message(uint8_t *message);

// ─── Distributed Control ──────────────────────────────────────────────────────
// Appelées depuis withrottle_if.c pour chaque commande Engine Driver

// Commande de traction : vitesse + direction + fonctions F0..F28
// speed = 0..126, ou -1 pour emergency stop
void bidib_send_cs_drive(uint16_t dcc_addr, int speed, uint8_t dir,
                         int f[29]);

// Alimentation voie : on=1 (PPA1), on=0 (PPA0)
void bidib_send_boost_state(uint8_t on);

#endif /* BIDIB_CLIENT_PARSER_H_ */
