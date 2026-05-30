/*
 * withrottle_if.h  —  Pico 2W
 */
#ifndef WITHROTTLE_IF_H_
#define WITHROTTLE_IF_H_

#include "datatypes.h"

// Traitement d'un message WiThrottle reçu
// slot = index dans throttle[] (passé directement depuis tcp_server_recv_cb)
void process_rx_withrottle(rx_data_t *data, uint8_t slot);

// Arrêt d'urgence d'un throttle (appelé sur déconnexion)
void throttle_stop(uint8_t slot);

#endif /* WITHROTTLE_IF_H_ */