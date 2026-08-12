/*
 * tcp_server.h  —  Pico 2W
 */
#ifndef TCP_SERVER_H_
#define TCP_SERVER_H_

#include <stdbool.h>
#include "lwip/tcp.h"

// Initialisation WiFi : STA (réseau existant) avec fallback AP
bool wifi_init(void);

// Démarrage du serveur TCP WiThrottle
bool tcp_server_init(void);

// Envoi d'un message TCP vers un client
// Équivalent send_msg(sock, len, msg) ESP32
void send_msg(struct tcp_pcb *pcb, int len, const char *msg);

#endif /* TCP_SERVER_H_ */