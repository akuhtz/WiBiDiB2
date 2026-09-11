/*
 * http_server.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Minimal HTTP server for EngineDriver roster access.
 * Uses raw lwIP callbacks (same pattern as tcp_server.c).
 * Serves GET /roster/?format=xml → JMRI-compatible roster XML.
 *
 * Design:
 *   - One connection at a time (Pico RAM constraints)
 *   - ALL XML generation is resumable via a single phase counter.
 *     If tcp_write returns ERR_MEM, the current chunk is retried
 *     on the next poll — no data is skipped.
 *   - Connection closed after response (no keep-alive)
 */

#include <string.h>
#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "config.h"
#include "http_server.h"
#include "roster.h"

static const char *TAG = "http";

// ─── State for one HTTP connection ────────────────────────────────────────
typedef enum {
    HTTP_IDLE,
    HTTP_SENDING_XML,
} http_state_t;

typedef struct {
    struct tcp_pcb *pcb;
    http_state_t   state;
    uint8_t        loco_index;     // next loco to send
    uint8_t        phase;          // sub-phase within XML generation
} http_conn_t;

static http_conn_t http_conn;                // single connection state
static struct tcp_pcb *http_listen_pcb;      // listening PCB

// ─── Phase numbering ──────────────────────────────────────────────────────
// Every chunk of output gets its own phase number, so a partial send
// due to ERR_MEM can be retried on the next poll without skipping data.
//
//   0   HTTP response header
//   1   XML declaration
//   2   <roster-config ...> part 1
//   3   <roster-config ...> part 2
//   4   <roster>
//   5+  Per-loco phases (5 phases per loco, offset by LOCO_PHASE_BASE)
//       +0  locomotive opening tag
//       +1  (decoder — skipped, placeholder)
//       +2  locoaddress
//       +3  functionlabels
//       +4  </locomotive>
//   N-1 </roster></roster-config>
//   N   close connection

#define LOCO_PHASE_BASE  5
#define LOCO_PHASES_PER  5

// ─── XML write helpers ────────────────────────────────────────────────────

static err_t xml_write(struct tcp_pcb *pcb, const char *data, u16_t len) {
    err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) tcp_output(pcb);
    return err;
}

static err_t xml_write_str(struct tcp_pcb *pcb, const char *s) {
    return xml_write(pcb, s, (u16_t)strlen(s));
}

// Write XML-escaped string (handle & and < only — sufficient for roster data)
static err_t xml_write_escaped(struct tcp_pcb *pcb, const char *s) {
    const char *p = s;
    while (*p) {
        if (*p == '&') {
            err_t r = xml_write(pcb, "&amp;", 5);
            if (r != ERR_OK) return r;
        } else if (*p == '<') {
            err_t r = xml_write(pcb, "&lt;", 4);
            if (r != ERR_OK) return r;
        } else {
            err_t r = xml_write(pcb, p, 1);
            if (r != ERR_OK) return r;
        }
        p++;
    }
    return ERR_OK;
}

// ─── Compute the total number of phases ───────────────────────────────────
//   LOCO_PHASE_BASE + roster.count * LOCO_PHASES_PER + 2 (closing + done)
static uint8_t xml_total_phases(void) {
    return LOCO_PHASE_BASE + roster.count * LOCO_PHASES_PER + 2;
}

// ─── Resumable XML generation ─────────────────────────────────────────────
// Returns true when the entire response has been sent and the connection
// is closed (or being closed).  Returns false if we need another poll.
static bool xml_send_chunk(struct tcp_pcb *pcb) {
    char buf[128];
    uint8_t p = http_conn.phase;

    // ── Phase 0: HTTP response header ──────────────────────────────────
    if (p == 0) {
        if (xml_write_str(pcb,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/xml; charset=utf-8\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Connection: close\r\n"
                "\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Phase 1: XML declaration ───────────────────────────────────────
    if (p == 1) {
        log_snprintf(buf, sizeof(buf),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n");
        if (xml_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Phase 2: <roster-config ...> part 1 ────────────────────────────
    if (p == 2) {
        log_snprintf(buf, sizeof(buf),
            "<roster-config xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" ");
        if (xml_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Phase 3: <roster-config ...> part 2 ────────────────────────────
    if (p == 3) {
        log_snprintf(buf, sizeof(buf),
            "xsi:noNamespaceSchemaLocation=\"http://jmri.org/xml/schema/roster.xsd\">\r\n");
        if (xml_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Phase 4: <roster> ──────────────────────────────────────────────
    if (p == 4) {
        log_snprintf(buf, sizeof(buf), "<roster>\r\n");
        if (xml_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Per-loco phases ────────────────────────────────────────────────
    if (p >= LOCO_PHASE_BASE && p < LOCO_PHASE_BASE + roster.count * LOCO_PHASES_PER) {
        uint8_t idx = (p - LOCO_PHASE_BASE) / LOCO_PHASES_PER;
        uint8_t lp  = (p - LOCO_PHASE_BASE) % LOCO_PHASES_PER;

        const roster_entry_t *e = roster_get(idx);
        if (!e) {
            // skip missing entry — advance past all its phases
            http_conn.phase = LOCO_PHASE_BASE + (idx + 1) * LOCO_PHASES_PER;
            return false;
        }

        switch (lp) {
        case 0:  // locomotive opening tag
            log_snprintf(buf, sizeof(buf),
                "<locomotive id=\"%s\" roadNumber=\"%s\" roadName=\"%s\" "
                "mfg=\"%s\" model=\"%s\" dccAddress=\"%d\" ",
                e->id, e->roadNumber, e->roadName,
                e->mfg, e->model, e->dccAddress);
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            log_snprintf(buf, sizeof(buf),
                "maxSpeed=\"%d\">",
                e->maxSpeed);
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            http_conn.phase = ++p;
            return false;

        case 1:  // decoder (skipped — placeholder)
            http_conn.phase = ++p;
            return false;

        case 2:  // locoaddress
            log_snprintf(buf, sizeof(buf),
                "<locoaddress><dcclocoaddress number=\"%d\" longaddress=\"%s\"/>",
                e->dccAddress, e->longAddress ? "true" : "false");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            log_snprintf(buf, sizeof(buf),
                "<number>%d</number>"
                "<protocol>%s</protocol>",
                e->dccAddress, e->longAddress ? "dcc_long" : "dcc_short");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            log_snprintf(buf, sizeof(buf),
                "</locoaddress>");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            http_conn.phase = ++p;
            return false;

        case 3: {  // functionlabels
            log_snprintf(buf, sizeof(buf), "<functionlabels>");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            for (uint8_t fn = 0; fn <= e->maxFnNum && fn <= ROSTER_FUNC_MAX; fn++) {
                if (e->functions[fn].label[0] == '\0') continue;
                log_snprintf(buf, sizeof(buf),
                    "<functionlabel num=\"%d\" lockable=\"%s\" visible=\"%s\">",
                    fn,
                    e->functions[fn].lockable ? "true" : "false",
                    e->functions[fn].visible  ? "true" : "false");
                if (xml_write_str(pcb, buf) != ERR_OK) return false;

                log_snprintf(buf, sizeof(buf),
                    e->functions[fn].label);
                if (xml_write_escaped(pcb, buf) != ERR_OK) return false;

                log_snprintf(buf, sizeof(buf),
                    "</functionlabel>");
                if (xml_write_str(pcb, buf) != ERR_OK) return false;
            }

            log_snprintf(buf, sizeof(buf), "</functionlabels>");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            http_conn.phase = ++p;
            return false;
        }

        case 4:  // </locomotive>
            log_snprintf(buf, sizeof(buf), "</locomotive>");
            if (xml_write_str(pcb, buf) != ERR_OK) return false;

            http_conn.phase = ++p;
            return false;
        }
    }

    // ── Closing tags ───────────────────────────────────────────────────
    if (p == LOCO_PHASE_BASE + roster.count * LOCO_PHASES_PER) {
        log_snprintf(buf, sizeof(buf),
            "</roster>\r\n</roster-config>\r\n");
        if (xml_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // ── Done — close connection ────────────────────────────────────────
    xml_write_str(pcb, "\r\n");
    tcp_close(pcb);
    http_conn.pcb   = NULL;
    http_conn.state = HTTP_IDLE;
    return true;
}

// ─── Parse HTTP request (simplified) ─────────────────────────────────────
static bool is_roster_request(const char *request) {
    return (strstr(request, "GET /roster") != NULL);
}

// ─── lwIP callbacks ──────────────────────────────────────────────────────
static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p) {
        if (http_conn.pcb == pcb) {
            http_conn.pcb   = NULL;
            http_conn.state = HTTP_IDLE;
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }

    char request[128];
    u16_t copy_len = (p->tot_len < sizeof(request) - 1) ? p->tot_len : sizeof(request) - 1;
    pbuf_copy_partial(p, request, copy_len, 0);
    request[copy_len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    LOG_INFO(TAG, "HTTP request: %s", request);

    if (http_conn.pcb != NULL && http_conn.pcb != pcb) {
        tcp_close(pcb);
        return ERR_OK;
    }

    if (is_roster_request(request)) {
        http_conn.pcb   = pcb;
        http_conn.state = HTTP_SENDING_XML;
        http_conn.loco_index = 0;
        http_conn.phase = 0;
        xml_send_chunk(pcb);
    } else {
        xml_write_str(pcb,
            "HTTP/1.1 404 Not Found\r\n"
            "Connection: close\r\n"
            "\r\n");
        tcp_close(pcb);
    }

    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err) {
    LOG_WARN(TAG, "HTTP TCP error: %d", err);
    if (http_conn.pcb) {
        http_conn.pcb   = NULL;
        http_conn.state = HTTP_IDLE;
    }
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb) {
    if (http_conn.pcb == pcb && http_conn.state == HTTP_SENDING_XML) {
        xml_send_chunk(pcb);
    }
    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    if (http_conn.pcb != NULL) {
        LOG_WARN(TAG, "HTTP: rejecting second connection");
        tcp_close(newpcb);
        return ERR_OK;
    }

    tcp_setprio(newpcb, TCP_PRIO_MIN);

    memset(&http_conn, 0, sizeof(http_conn));

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv_cb);
    tcp_err(newpcb, http_err_cb);
    tcp_poll(newpcb, http_poll_cb, 2);

    LOG_INFO(TAG, "HTTP connection accepted");
    return ERR_OK;
}

// ─── Public API ──────────────────────────────────────────────────────────
bool http_server_init(void) {
    http_listen_pcb = tcp_new();
    if (!http_listen_pcb) {
        LOG_ERROR(TAG, "Failed to allocate HTTP listen PCB");
        return false;
    }

    err_t err = tcp_bind(http_listen_pcb, IP_ADDR_ANY, HTTP_ROSTER_PORT);
    if (err != ERR_OK) {
        LOG_ERROR(TAG, "Failed to bind HTTP on port %d: %d", HTTP_ROSTER_PORT, err);
        tcp_close(http_listen_pcb);
        http_listen_pcb = NULL;
        return false;
    }

    http_listen_pcb = tcp_listen(http_listen_pcb);
    if (!http_listen_pcb) {
        LOG_ERROR(TAG, "Failed to listen on HTTP port %d", HTTP_ROSTER_PORT);
        return false;
    }

    tcp_accept(http_listen_pcb, http_accept_cb);

    LOG_INFO(TAG, "HTTP roster server listening on port %d", HTTP_ROSTER_PORT);
    return true;
}
