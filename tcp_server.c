/*
 * tcp_server.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Portage depuis ESP32 (FreeRTOS + BSD sockets) → Pico bare-metal (lwIP callbacks)
 *
 * Architecture :
 *   ESP32 : thread bloquant recv() dans une FreeRTOS task
 *   Pico  : callbacks lwIP appelés par cyw43_arch_poll() dans la boucle principale
 *
 *   tcp_server_accept_cb()   ← nouvelle connexion
 *   tcp_server_recv_cb()     ← données reçues  → process_rx_withrottle()
 *   tcp_server_err_cb()      ← erreur / déconnexion
 *
 * Pierre Moulin — portage 2025
 */

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "config.h"
#include "datatypes.h"
#include "withrottle_if.h"
#include "smartphone_if.h"
#include "dhcpserver/dhcpserver.h"
#include "mdns.h"

static const char *TAG = "tcp_server";

// ─── Prototypes internes ──────────────────────────────────────────────────────
static err_t tcp_server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void  tcp_server_err_cb(void *arg, err_t err);

static bool wifi_try_sta(void);
static bool wifi_start_ap(void);

// Interface réseau active (STA si connecté, sinon AP) — utilisée par mDNS
static struct netif *g_active_netif;

// ─── Initialisation WiFi ──────────────────────────────────────────────────────
//
// Mode par défaut : STA (rejoint un réseau WiFi existant, IP via DHCP).
// En cas d'échec/timeout → fallback en mode AP (comportement d'origine).
//
// Le Pico 2W utilise cyw43_arch
//
bool wifi_init(void) {
    if (cyw43_arch_init()) {
        LOG_ERROR(TAG, "cyw43_arch_init failed");
        return false;
    }

    if (wifi_try_sta()) {
        return true;
    }

    LOG_WARN(TAG, "STA connect failed -- falling back to AP mode");
    return wifi_start_ap();
}

// ─── Mode STA : rejoint un réseau WiFi existant ───────────────────────────────
static bool wifi_try_sta(void) {
    cyw43_arch_enable_sta_mode();

    LOG_INFO(TAG, "Connecting to WiFi SSID: %s ...", WIFI_SSID);
    int ret = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                                 CYW43_AUTH_WPA2_AES_PSK,
                                                 WIFI_STA_TIMEOUT_MS);
    if (ret != 0) {
        LOG_ERROR(TAG, "STA connect failed, error %d", ret);
        return false;
    }

    // Attendre l'attribution de l'IP (DHCP — démarré automatiquement par le SDK)
    struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
    uint32_t start = now_ms();
    while (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP
        || ip4_addr_isany_val(*netif_ip4_addr(sta_netif))) {
        if (now_ms() - start > WIFI_STA_TIMEOUT_MS) {
            LOG_ERROR(TAG, "STA DHCP/IP timeout");
            return false;
        }
        cyw43_arch_poll();
        sleep_ms(100);
    }

    LOG_INFO(TAG, "WiFi STA connected. IP: %s", ip4addr_ntoa(netif_ip4_addr(sta_netif)));
    g_active_netif = sta_netif;
    return true;
}

// ─── Mode AP : le Pico démarre son propre réseau ──────────────────────────────
static bool wifi_start_ap(void) {
    cyw43_arch_enable_ap_mode(WIFI_AP_SSID, WIFI_AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t gw, mask;
    ip4addr_aton(AP_IP_ADDR, &gw);
    ip4addr_aton("255.255.255.0", &mask);

    // Configurer l'IP de l'interface AP
    struct netif *ap_netif = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(ap_netif, &gw, &mask, &gw);

    // DHCP server
    static dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, (ip_addr_t*)&gw, (ip_addr_t*)&mask);

    LOG_INFO(TAG, "WiFi AP started. SSID: %s, IP: %s", WIFI_AP_SSID, ip4addr_ntoa(&gw));
    g_active_netif = ap_netif;
    return true;
}

// ─── Démarrage du serveur TCP ─────────────────────────────────────────────────
//
// Équivalent du bloc listen/bind/accept dans tcp_server_task() ESP32
// Sur Pico lwIP : on crée un pcb en écoute et on enregistre le callback accept
//
bool tcp_server_init(void) {
    struct tcp_pcb *listen_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!listen_pcb) {
        LOG_ERROR(TAG, "tcp_new failed");
        return false;
    }

    err_t err = tcp_bind(listen_pcb, IP_ANY_TYPE, WITHROTTLE_PORT);
    if (err != ERR_OK) {
        LOG_ERROR(TAG, "tcp_bind failed: %d", err);
        tcp_close(listen_pcb);
        return false;
    }

    listen_pcb = tcp_listen_with_backlog(listen_pcb, MAX_CLIENTS);
    if (!listen_pcb) {
        LOG_ERROR(TAG, "tcp_listen failed");
        return false;
    }

    tcp_accept(listen_pcb, tcp_server_accept_cb);
    LOG_INFO(TAG, "TCP server listening on port %d", WITHROTTLE_PORT);

    // mDNS : annonce le service WiThrottle → découverte Engine Driver
    mdns_init(g_active_netif);
    return true;
}

// ─── Callback : nouvelle connexion acceptée ───────────────────────────────────
//
// Équivalent du bloc après accept() + xTaskCreate(process_socket_V2) ESP32
// Ici pas de thread : on enregistre recv/err callbacks sur le nouveau pcb
//
static err_t tcp_server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    // Priorité TCP (recommandé pico-sdk)
    tcp_setprio(newpcb, TCP_PRIO_MIN);

    // Trouver un slot libre dans la table des throttles
    uint8_t slot = find_free_throttle_slot();
    if (slot == 255) {
        LOG_WARN(TAG, "Too many clients, refusing connection");
        tcp_close(newpcb);
        return ERR_MEM;
    }

    // Enregistrer le client
    throttle[slot].pcb   = newpcb;
    throttle[slot].state = NODE_LOGGED_ON;
    if (cli_index < MAX_THROTTLES - 1) cli_index++;

    LOG_INFO(TAG, "Client connected, slot %d, pcb %p", slot, newpcb);

    // Enregistrer les callbacks sur ce pcb
    tcp_arg(newpcb, (void *)(uintptr_t)slot);   // arg = index dans throttle[]
    tcp_recv(newpcb, tcp_server_recv_cb);
    tcp_err(newpcb, tcp_server_err_cb);


    return ERR_OK;
}


// ─── Callback : données reçues ────────────────────────────────────────────────
//
// Équivalent du recv() bloquant dans process_socket_V2() ESP32
// + xQueueSend() + parse_rx_smart_if_task() + process_rx_withrottle()
//
// Sur Pico : appel direct (pas de queue), tout dans le même contexte poll()
//
static err_t tcp_server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    uint8_t slot = (uint8_t)(uintptr_t)arg;

    if (!p) {
        // p == NULL → connexion fermée par le client
        LOG_INFO(TAG, "Connection closed by client, slot %d", slot);
        throttle_stop(slot);
        throttle[slot].state = NODE_INACTIVE;
        throttle[slot].pcb   = NULL;
        tcp_close(tpcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    // Copier les données du pbuf dans rx_data_t
    // Le pbuf peut être chaîné (plusieurs segments) — pbuf_copy_partial gère ça
    rx_data_t rx;
    rx.pcb = tpcb;
    rx.len = (int)p->tot_len;
    if (rx.len > (int)(sizeof(rx.msg) - 1)) rx.len = sizeof(rx.msg) - 1;

    pbuf_copy_partial(p, rx.msg, rx.len, 0);
    rx.msg[rx.len] = '\0';   // null-terminate comme sur ESP32

    LOG_INFO(TAG, "<- slot %d; Received %d bytes: %s", slot, rx.len, rx.msg);

    // Acquitter la réception (obligatoire lwIP)
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Split sur \n — un paquet peut contenir plusieurs messages
    // ex: "M0A*<;>qV\nM0A*<;>qR\n"
    char *line_start = rx.msg;
    char *newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';  // terminer la ligne
        int line_len = (int)(newline - line_start);

        // Ignorer les lignes vides
        if (line_len > 0) {
            rx_data_t line_rx;
            line_rx.pcb = tpcb;
            line_rx.len = line_len;
            memcpy(line_rx.msg, line_start, line_len + 1);
            process_rx_withrottle(&line_rx, slot);
        }
        line_start = newline + 1;
    }

    // Traiter le reste sans \n final (si présent)
    if (*line_start != '\0') {
        rx_data_t line_rx;
        line_rx.pcb = tpcb;
        line_rx.len = (int)strlen(line_start);
        memcpy(line_rx.msg, line_start, line_rx.len + 1);
        process_rx_withrottle(&line_rx, slot);
    }
    return ERR_OK;
}

// ─── Callback : erreur TCP ────────────────────────────────────────────────────
//
// Équivalent du errno check + shutdown/close dans process_socket_V2() ESP32
// Note : quand err_cb est appelé, le pcb est déjà invalide (ne pas appeler tcp_close)
//
static void tcp_server_err_cb(void *arg, err_t err) {
    uint8_t slot = (uint8_t)(uintptr_t)arg;
    LOG_WARN(TAG, "TCP error on slot %d, err=%d", slot, err);
    if (slot < MAX_THROTTLES) {
        throttle_stop(slot);
        throttle[slot].state = NODE_INACTIVE;
        throttle[slot].pcb   = NULL;
    }
}

// ─── send_msg() ───────────────────────────────────────────────────────────────
//
// Équivalent de send_msg(sock, len, msg) ESP32
// Utilisé par withrottle_if.c — signature identique sauf pcb au lieu de sock
//
void send_msg(struct tcp_pcb *pcb, int len, const char *msg) {
    if (!pcb || len <= 0) return;
    LOG_INFO(TAG, "-> msg len %d bytes: %s",  len, msg);

    err_t err = tcp_write(pcb, msg, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("[send_msg] tcp_write error: %d len=%d\n", err, len);
        return;
    }
    // PAS de tcp_output() ici — flush explicite après chaque bloc
}
