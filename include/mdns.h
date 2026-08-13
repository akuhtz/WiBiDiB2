/*
 * mdns.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Respondant mDNS (lwIP apps/mdns) : annonce le service _withrottle._tcp
 * pour que l'appli Engine Driver découvre le gateway automatiquement.
 */

#ifndef MDNS_H_
#define MDNS_H_

#include <stdbool.h>

struct netif;

// Enregistre le gateway comme service WiThrottle sur l'interface donnée.
// À appeler après que le netif a une adresse IP.
bool mdns_init(struct netif *netif);

#endif /* MDNS_H_ */