/*
 * mdns.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Respondant mDNS basé sur lwIP (pico_lwip_mdns). Annonce le service
 * _withrottle._tcp.local. → Engine Driver découvre le gateway sans
 * saisie manuelle d'IP/port.
 *
 * Séquence obligatoire (SDK 2.2.0) : mdns_resp_init() puis
 * mdns_resp_add_netif() avant mdns_resp_add_service() — sinon hardfault
 * dans mdns_build_service_domain().
 */

#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#include "config.h"
#include "mdns.h"

static const char *TAG = "mdns";

// Nom d'hôte (A record) et instance du service WiThrottle
#define MDNS_HOSTNAME    "wibidib"
#define MDNS_INSTANCE    "WiBiDiB"
#define MDNS_SERVICE     "_withrottle"
#define MDNS_PROTO       DNSSD_PROTO_TCP

bool mdns_init(struct netif *netif) {
    if (!netif) {
        LOG_ERROR(TAG, "mdns_init: netif NULL");
        return false;
    }

    cyw43_arch_lwip_begin();

    mdns_resp_init();

    if (!mdns_resp_netif_active(netif)) {
        err_t r = mdns_resp_add_netif(netif, MDNS_HOSTNAME);
        if (r != ERR_OK) {
            LOG_ERROR(TAG, "mdns_resp_add_netif failed: %d", r);
            cyw43_arch_lwip_end();
            return false;
        }
    }

    s8_t slot = mdns_resp_add_service(netif, MDNS_INSTANCE,
                                      MDNS_SERVICE, MDNS_PROTO,
                                      WITHROTTLE_PORT, NULL, NULL);
    if (slot < 0) {
        LOG_ERROR(TAG, "mdns_resp_add_service failed: %d", slot);
        cyw43_arch_lwip_end();
        return false;
    }

    mdns_resp_announce(netif);

    cyw43_arch_lwip_end();

    LOG_INFO(TAG, "mDNS up: %s._withrottle._tcp.local. port %d on %s",
             MDNS_INSTANCE, WITHROTTLE_PORT, ip4addr_ntoa(netif_ip4_addr(netif)));
    return true;
}