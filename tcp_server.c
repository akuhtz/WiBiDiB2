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
#include "FreeRTOS.h"
#include "task.h"

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

// ─── STA mode: joins an existing WiFi network ───────────────────────────────
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

    LOG_INFO(TAG, "Connecting to WiFi SSID passed: %s ...", WIFI_SSID);

    // Wait for IP assignment (DHCP — handled by lwIP tcpip_thread)
    struct netif *sta_netif = &cyw43_state.netif[CYW43_ITF_STA];
    uint32_t start = now_ms();
    while (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP
        || ip4_addr_isany_val(*netif_ip4_addr(sta_netif))) {
        if (now_ms() - start > WIFI_STA_TIMEOUT_MS) {
            LOG_ERROR(TAG, "STA DHCP/IP timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    LOG_INFO(TAG, "WiFi STA connected. IP: %s", ip4addr_ntoa(netif_ip4_addr(sta_netif)));
    g_active_netif = sta_netif;
    return true;
}

// ─── AP mode: Pico starts its own network ──────────────────────────────
static bool wifi_start_ap(void) {
    cyw43_arch_enable_ap_mode(WIFI_AP_SSID, WIFI_AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t gw, mask;
    ip4addr_aton(AP_IP_ADDR, &gw);
    ip4addr_aton("255.255.255.0", &mask);

    // Configure the IP of the AP interface
    struct netif *ap_netif = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(ap_netif, &gw, &mask, &gw);

    // DHCP server
    static dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, (ip_addr_t*)&gw, (ip_addr_t*)&mask);

    LOG_INFO(TAG, "WiFi AP started. SSID: %s, IP: %s", WIFI_AP_SSID, ip4addr_ntoa(&gw));
    g_active_netif = ap_netif;
    return true;
}

// ─── TCP server startup ─────────────────────────────────────────────────
//
// Equivalent of the listen/bind/accept block in tcp_server_task() ESP32
// On Pico lwIP: we create a pcb for listening and register the accept callback
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

    // mDNS : advertise the WiThrottle service → Engine Driver discovery
    mdns_init(g_active_netif);
    return true;
}

// ─── Callback: new connection accepted ───────────────────────────────────
//
// Equivalent of the block after accept() + xTaskCreate(process_socket_V2) ESP32
// No thread here : we register recv/err callbacks on the new pcb
//
static err_t tcp_server_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    // TCP priority (recommended by pico-sdk)
    tcp_setprio(newpcb, TCP_PRIO_MIN);

    // Find a free slot in the throttle table
    uint8_t slot = find_free_throttle_slot();
    if (slot == 255) {
        LOG_WARN(TAG, "Too many clients, refusing connection");
        tcp_close(newpcb);
        return ERR_MEM;
    }

    // Register the client
    throttle[slot].pcb   = newpcb;
    throttle[slot].state = NODE_LOGGED_ON;
    throttle[slot].welcome_sent = false;
    throttle[slot].throttleId = 0;
    if (cli_index < MAX_THROTTLES - 1) cli_index++;

    LOG_INFO(TAG, "Client connected, slot %d, pcb %p", slot, newpcb);

    // Register the callbacks on this pcb
    tcp_arg(newpcb, (void *)(uintptr_t)slot);   // arg = index dans throttle[]
    tcp_recv(newpcb, tcp_server_recv_cb);
    tcp_err(newpcb, tcp_server_err_cb);

    return ERR_OK;
}


// ─── Callback: data received ────────────────────────────────────────────────
//
// Equivalent of the blocking recv() in process_socket_V2() ESP32
// + xQueueSend() + parse_rx_smart_if_task() + process_rx_withrottle()
//
// On Pico: direct call (no queue), all in the same poll() context
//
static err_t tcp_server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    uint8_t slot = (uint8_t)(uintptr_t)arg;

    if (!p) {
        // p == NULL → connection closed by client
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

    // Copy data from pbuf into rx_data_t
    // The pbuf may be chained (multiple segments) — pbuf_copy_partial handles this
    rx_data_t rx;
    rx.pcb = tpcb;
    rx.len = (int)p->tot_len;
    if (rx.len > (int)(sizeof(rx.msg) - 1)) rx.len = sizeof(rx.msg) - 1;

    pbuf_copy_partial(p, rx.msg, rx.len, 0);
    rx.msg[rx.len] = '\0';   // null-terminate comme sur ESP32

    LOG_INFO(TAG, "rcv: <- slot %d; Received %d bytes: %s", slot, rx.len, rx.msg);

    // Acknowledge reception (mandatory lwIP)
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);

    // Send welcome on first data from client (safe here, unlike accept_cb)
    if (!throttle[slot].welcome_sent) {
        throttle[slot].welcome_sent = true;

        LOG_INFO(TAG, "Send welcome message to new client");
        send_welcome_message(tpcb);
    }

    // Split on \n — a packet may contain multiple messages
    // ex: "M0A*<;>qV\nM0A*<;>qR\n"
    char *line_start = rx.msg;
    char *newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';  // terminer la ligne
        int line_len = (int)(newline - line_start);

        // Ignore empty lines
        if (line_len > 0) {
            rx_data_t line_rx;
            line_rx.pcb = tpcb;
            line_rx.len = line_len;
            memcpy(line_rx.msg, line_start, line_len + 1);
            process_rx_withrottle(&line_rx, slot);
        }
        line_start = newline + 1;
    }

    // Process the remaining data without \n final (if present)
    if (*line_start != '\0') {
        rx_data_t line_rx;
        line_rx.pcb = tpcb;
        line_rx.len = (int)strlen(line_start);
        memcpy(line_rx.msg, line_start, line_rx.len + 1);
        process_rx_withrottle(&line_rx, slot);
    }
    return ERR_OK;
}

// ─── Callback: TCP error ────────────────────────────────────────────────────
//
// Equivalent of the errno check + shutdown/close in process_socket_V2() ESP32
// Note : when err_cb is called, the pcb is already invalid (do not call tcp_close)
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
// Equivalent of send_msg(sock, len, msg) ESP32
// Used by withrottle_if.c — identical signature except pcb instead of sock
//
void send_msg(struct tcp_pcb *pcb, int len, const char *msg) {
    if (!pcb || len <= 0) return;
    LOG_INFO(TAG, "snd: -> msg len %d bytes: %s",  len, msg);

    err_t err = tcp_write(pcb, msg, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("[send_msg] tcp_write error: %d len=%d\n", err, len);
        return;
    }
    // NO tcp_output() here — explicit flush after each block
}
