/*
 * http_server.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Minimal HTTP server for:
 *   - GET /roster/           JMRI-compatible roster XML (EngineDriver)
 *   - GET /                  redirect to /roster.html
 *   - GET /roster.html       HTML table + Add / Edit / Delete controls
 *   - GET /edit?slot=N       HTML form pre-filled from roster entry N
 *   - GET /edit?slot=new     blank HTML form for a new entry
 *   - POST /save             parse form, roster_add/update, redirect
 *   - POST /delete?slot=N    delete entry N, redirect
 *
 * Design:
 *   - Single connection at a time (Pico RAM constraints)
 *   - All response generation resumable via a phase counter
 *   - HTML skeleton chunks in .rodata (flash-resident, no RAM cost)
 *   - POST body accumulated into http_conn.post_buf until complete
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "config.h"
#include "http_server.h"
#include "roster.h"
#include "form_parser.h"

static const char *TAG = "http";

// ─── Routes / methods ─────────────────────────────────────────────────────
typedef enum {
    R_UNKNOWN = 0,
    R_INDEX,          // GET /
    R_ROSTER_HTML,    // GET /roster.html
    R_ROSTER_XML,     // GET /roster/, /roster
    R_EDIT,           // GET /edit?slot=N
    R_SAVE,           // POST /save
    R_DELETE,         // POST /delete?slot=N
    R_400,            // Bad request
    R_404,            // Not found
} http_route_t;

typedef enum { M_GET = 0, M_POST } http_method_t;

typedef enum {
    HTTP_IDLE,
    HTTP_RECV_HEADERS,      // parsing status line + headers
    HTTP_RECV_BODY,         // accumulating POST body
    HTTP_SENDING,           // response generator running
} http_state_t;

#define POST_BUF_SIZE   3072

typedef struct {
    struct tcp_pcb *pcb;
    http_state_t   state;
    http_method_t  method;
    http_route_t   route;

    // Query / form context
    uint16_t       arg_slot;         // parsed from ?slot=N; 0xFFFF = "new"; 0xFFFE = missing
    uint16_t       phase;            // response state machine
    uint16_t       loco_dense_idx;   // for XML iteration
    uint8_t        loco_slot;        // physical slot of the entry being emitted
    uint8_t        loco_fn;          // function index within a per-loco phase
    roster_entry_t entry;            // snapshot for XML/HTML; or form output for POST

    // POST body buffer
    uint16_t       post_len;
    uint16_t       post_expected;
    bool           headers_done;
    char           post_buf[POST_BUF_SIZE];
} http_conn_t;

#define SLOT_MISSING   0xFFFE
#define SLOT_NEW       0xFFFF

static http_conn_t http_conn;
static struct tcp_pcb *http_listen_pcb;

// Shared scratch buffer used by response generators. Safe because only
// one HTTP connection is served at a time and generators never re-enter.
// Sized for the largest chunk (html_edit_send: 768 bytes).
static char g_gen_buf[768];

// ─── Low-level write helpers ─────────────────────────────────────────────
// http_write_buffered queues data into lwIP's send buffer without pushing
// it out immediately. Call http_flush() once at the end of a generator
// invocation to send everything as few TCP segments as possible.
static err_t http_write_buffered(struct tcp_pcb *pcb, const char *data, u16_t len) {
    return tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
}

static void http_flush(struct tcp_pcb *pcb) {
    tcp_output(pcb);
}

// Backward-compat: writes and flushes immediately (used by short single-shot
// responses like 302/400/404 where batching offers no gain).
static err_t http_write(struct tcp_pcb *pcb, const char *data, u16_t len) {
    err_t err = tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) tcp_output(pcb);
    return err;
}

static err_t http_write_str(struct tcp_pcb *pcb, const char *s) {
    return http_write_buffered(pcb, s, (u16_t)strlen(s));
}

// Write XML-escaped string (handles & and <)
static err_t http_write_xml_escaped(struct tcp_pcb *pcb, const char *s) {
    while (*s) {
        if (*s == '&') {
            err_t r = http_write_buffered(pcb, "&amp;", 5);
            if (r != ERR_OK) return r;
        } else if (*s == '<') {
            err_t r = http_write_buffered(pcb, "&lt;", 4);
            if (r != ERR_OK) return r;
        } else {
            err_t r = http_write_buffered(pcb, s, 1);
            if (r != ERR_OK) return r;
        }
        s++;
    }
    return ERR_OK;
}

// Write HTML-escaped string (handles &, <, >, ", ')
static err_t http_write_html_escaped(struct tcp_pcb *pcb, const char *s) {
    while (*s) {
        const char *rep = NULL; u16_t rl = 0;
        switch (*s) {
        case '&':  rep = "&amp;";  rl = 5; break;
        case '<':  rep = "&lt;";   rl = 4; break;
        case '>':  rep = "&gt;";   rl = 4; break;
        case '"':  rep = "&quot;"; rl = 6; break;
        case '\'': rep = "&#39;";  rl = 5; break;
        }
        err_t r;
        if (rep) r = http_write_buffered(pcb, rep, rl);
        else     r = http_write_buffered(pcb, s, 1);
        if (r != ERR_OK) return r;
        s++;
    }
    return ERR_OK;
}

// ─── Connection reset ────────────────────────────────────────────────────
static void conn_reset(void) {
    http_conn.pcb           = NULL;
    http_conn.state         = HTTP_IDLE;
    http_conn.phase         = 0;
    http_conn.post_len      = 0;
    http_conn.post_expected = 0;
    http_conn.headers_done  = false;
    http_conn.route         = R_UNKNOWN;
    http_conn.arg_slot      = SLOT_MISSING;
}

static void conn_close(struct tcp_pcb *pcb) {
    // Push any buffered response data before closing — tcp_close only
    // queues a FIN and does not flush pending tcp_write() bytes.
    tcp_output(pcb);
    if (http_conn.pcb == pcb) conn_reset();
    tcp_close(pcb);
}

// ─── Request line + header parsing ───────────────────────────────────────
// Parses the first line ("METHOD PATH HTTP/1.1") and the Content-Length
// header. Returns true when the double-CRLF end-of-headers is seen, so
// the caller knows whether to transition into HTTP_RECV_BODY or dispatch.
static uint16_t parse_slot_query(const char *q) {
    // q points to "slot=..." (after '?')
    const char *v = strstr(q, "slot=");
    if (!v) return SLOT_MISSING;
    v += 5;
    if (strncmp(v, "new", 3) == 0) return SLOT_NEW;
    unsigned long n = strtoul(v, NULL, 10);
    if (n >= ROSTER_MAX_ENTRIES) return SLOT_MISSING;
    return (uint16_t)n;
}

static void classify_request(char *line) {
    // line is "METHOD PATH HTTP/1.1"
    char *method = line;
    char *path   = strchr(line, ' ');
    if (!path) { http_conn.route = R_400; return; }
    *path++ = '\0';
    char *ver = strchr(path, ' ');
    if (ver) *ver = '\0';

    if (strcmp(method, "GET") == 0)       http_conn.method = M_GET;
    else if (strcmp(method, "POST") == 0) http_conn.method = M_POST;
    else { http_conn.route = R_400; return; }

    // Split path from query string
    char *q = strchr(path, '?');
    if (q) { *q++ = '\0'; }
    else   { q = ""; }

    if (http_conn.method == M_GET) {
        if (strcmp(path, "/") == 0)                          http_conn.route = R_INDEX;
        else if (strcmp(path, "/roster.html") == 0)          http_conn.route = R_ROSTER_HTML;
        else if (strncmp(path, "/roster", 7) == 0)           http_conn.route = R_ROSTER_XML;
        else if (strcmp(path, "/edit") == 0) {
            http_conn.route = R_EDIT;
            http_conn.arg_slot = parse_slot_query(q);
        }
        else http_conn.route = R_404;
    } else {
        if (strcmp(path, "/save") == 0)          http_conn.route = R_SAVE;
        else if (strcmp(path, "/delete") == 0) {
            http_conn.route = R_DELETE;
            http_conn.arg_slot = parse_slot_query(q);
        }
        else http_conn.route = R_404;
    }
}

// Process the request head data received so far. Returns true when
// end-of-headers has been reached and post_expected is known.
// Any body bytes past the CRLFCRLF are copied into post_buf.
static void process_headers(const char *data, u16_t len) {
    // We accumulate into post_buf until we see \r\n\r\n. This is a
    // simple approach that works for short requests (headers < 3 KB).
    for (u16_t i = 0; i < len && http_conn.post_len + 1 < POST_BUF_SIZE; i++) {
        http_conn.post_buf[http_conn.post_len++] = data[i];
        // Look for CRLFCRLF ending
        if (http_conn.post_len >= 4) {
            char *p = http_conn.post_buf;
            u16_t n = http_conn.post_len;
            if (p[n - 4] == '\r' && p[n - 3] == '\n' &&
                p[n - 2] == '\r' && p[n - 1] == '\n') {
                // Headers complete
                p[n - 4] = '\0';
                // Classify request line
                char *nl = strstr(p, "\r\n");
                if (nl) *nl = '\0';
                classify_request(p);
                if (nl) {
                    // Scan remaining headers for Content-Length
                    char *hdr = nl + 2;
                    while (hdr && *hdr) {
                        char *eol = strstr(hdr, "\r\n");
                        if (eol) *eol = '\0';
                        // Case-insensitive prefix match on "Content-Length:"
                        if (strncasecmp(hdr, "Content-Length:", 15) == 0) {
                            http_conn.post_expected = (uint16_t)strtoul(hdr + 15, NULL, 10);
                        }
                        if (!eol) break;
                        hdr = eol + 2;
                    }
                }
                http_conn.headers_done = true;
                // Any body bytes after the CRLFCRLF? Move them to the
                // start of post_buf (we consumed the header area).
                u16_t remaining = len - (i + 1);
                http_conn.post_len = 0;
                if (remaining > 0 && remaining < POST_BUF_SIZE) {
                    memcpy(http_conn.post_buf, data + i + 1, remaining);
                    http_conn.post_len = remaining;
                }
                return;
            }
        }
    }
}

// ─── HTML page fragments (in .rodata, flash-resident) ─────────────────────
static const char kHtmlHead[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>WiBiDiB2 Roster</title>"
    "<style>"
    "body{font-family:sans-serif;margin:16px;}"
    "table{border-collapse:collapse;}"
    "th,td{border:1px solid #ccc;padding:4px 8px;text-align:left;}"
    "th{background:#eee;}"
    "form.inline{display:inline;}"
    "a.btn,button{padding:2px 8px;margin:0 2px;}"
    ".fn-row td{padding:2px 4px;}"
    "</style></head><body>";

static const char kHtmlFoot[] =
    "</body></html>";

// ─── Response: 302 redirect ──────────────────────────────────────────────
static bool send_redirect(struct tcp_pcb *pcb, const char *location) {
    int n = snprintf(g_gen_buf, sizeof(g_gen_buf),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        location);
    (void)http_write(pcb, g_gen_buf, (u16_t)n);
    conn_close(pcb);
    return true;
}

// ─── Response: 404 ───────────────────────────────────────────────────────
static bool send_404(struct tcp_pcb *pcb) {
    static const char body[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 10\r\n"
        "Connection: close\r\n\r\n"
        "Not Found\n";
    http_write_str(pcb, body);
    conn_close(pcb);
    return true;
}

// ─── Response: 400 ───────────────────────────────────────────────────────
static bool send_400(struct tcp_pcb *pcb, const char *msg) {
    int n = snprintf(g_gen_buf, sizeof(g_gen_buf),
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n"
        "%s",
        (unsigned)strlen(msg), msg);
    (void)http_write(pcb, g_gen_buf, (u16_t)n);
    conn_close(pcb);
    return true;
}

// ─── /roster/ XML generator (phases 0..N) ────────────────────────────────
// Phase layout:
//   0    HTTP header
//   1    XML decl
//   2    <roster-config> open (part 1)
//   3    <roster-config> open (part 2)
//   4    <roster>
//   5+   per-loco: base + idx*LOCO_PHASES + [0..LOCO_PHASES-1]
//   last <roster>/<roster-config>/close
#define XML_LOCO_BASE     5
#define XML_LOCO_PHASES   5

static bool xml_send_chunk(struct tcp_pcb *pcb) {
    char *buf = g_gen_buf;
    uint16_t p = http_conn.phase;

    if (p == 0) {
        if (http_write_str(pcb,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/xml; charset=utf-8\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Connection: close\r\n\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 1) {
        if (http_write_str(pcb,
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 2) {
        if (http_write_str(pcb,
                "<roster-config xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" ") != ERR_OK)
            return false;
        http_conn.phase = ++p;
    }
    if (p == 3) {
        if (http_write_str(pcb,
                "xsi:noNamespaceSchemaLocation=\"http://jmri.org/xml/schema/roster.xsd\">\r\n") != ERR_OK)
            return false;
        http_conn.phase = ++p;
    }
    if (p == 4) {
        if (http_write_str(pcb, "<roster>\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    uint8_t count = roster_count_valid();
    while (p >= XML_LOCO_BASE && p < XML_LOCO_BASE + count * XML_LOCO_PHASES) {
        uint16_t idx = (p - XML_LOCO_BASE) / XML_LOCO_PHASES;
        uint16_t lp  = (p - XML_LOCO_BASE) % XML_LOCO_PHASES;

        if (lp == 0) {
            if (!roster_get_dense((uint8_t)idx, &http_conn.loco_slot, &http_conn.entry)) {
                http_conn.phase = XML_LOCO_BASE + (idx + 1) * XML_LOCO_PHASES;
                p = http_conn.phase;
                continue;
            }
        }
        const roster_entry_t *e = &http_conn.entry;

        switch (lp) {
        case 0:
            snprintf(buf, sizeof(g_gen_buf),
                "<locomotive id=\"%s\" roadNumber=\"%s\" roadName=\"%s\" "
                "mfg=\"%s\" model=\"%s\" dccAddress=\"%d\" maxSpeed=\"%d\">",
                e->id, e->roadNumber, e->roadName, e->mfg, e->model,
                e->dccAddress, e->maxSpeed);
            if (http_write_str(pcb, buf) != ERR_OK) return false;
            http_conn.phase = ++p;
            break;
        case 1:
            // decoder placeholder — skip
            http_conn.phase = ++p;
            break;
        case 2:
            snprintf(buf, sizeof(g_gen_buf),
                "<locoaddress><dcclocoaddress number=\"%d\" longaddress=\"%s\"/>"
                "<number>%d</number><protocol>%s</protocol></locoaddress>",
                e->dccAddress, e->longAddress ? "true" : "false",
                e->dccAddress, e->longAddress ? "dcc_long" : "dcc_short");
            if (http_write_str(pcb, buf) != ERR_OK) return false;
            http_conn.phase = ++p;
            break;
        case 3: {
            if (http_write_str(pcb, "<functionlabels>") != ERR_OK) return false;
            for (uint8_t fn = 0; fn <= e->maxFnNum && fn <= ROSTER_FUNC_MAX; fn++) {
                if (e->functions[fn].label[0] == '\0') continue;
                snprintf(buf, sizeof(g_gen_buf),
                    "<functionlabel num=\"%d\" lockable=\"%s\" visible=\"%s\">",
                    fn, e->functions[fn].lockable ? "true" : "false",
                    e->functions[fn].visible ? "true" : "false");
                if (http_write_str(pcb, buf) != ERR_OK) return false;
                if (http_write_xml_escaped(pcb, e->functions[fn].label) != ERR_OK) return false;
                if (http_write_str(pcb, "</functionlabel>") != ERR_OK) return false;
            }
            if (http_write_str(pcb, "</functionlabels>") != ERR_OK) return false;
            http_conn.phase = ++p;
            break;
        }
        case 4:
            if (http_write_str(pcb, "</locomotive>") != ERR_OK) return false;
            http_conn.phase = ++p;
            break;
        }
    }

    if (p == XML_LOCO_BASE + count * XML_LOCO_PHASES) {
        if (http_write_str(pcb,
                "</roster>\r\n</roster-config>\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // Done
    conn_close(pcb);
    return true;
}

// ─── /roster.html generator ──────────────────────────────────────────────
// Phase layout:
//   0    HTTP header + <html>...<body> + <h1> + <table> opening + header row
//   1    open loop marker
//   2+   per-entry rows: (2 + idx)
//   last table close + [Add] link + </body>
//   last+1 done
static bool html_table_send(struct tcp_pcb *pcb) {
    char *buf = g_gen_buf;
    uint16_t p = http_conn.phase;

    if (p == 0) {
        if (http_write_str(pcb,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Connection: close\r\n\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 1) {
        if (http_write_str(pcb, kHtmlHead) != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 2) {
        if (http_write_str(pcb,
                "<h1>WiBiDiB2 Roster</h1>"
                "<p><a class=\"btn\" href=\"/edit?slot=new\">[Add new]</a> "
                "<a href=\"/roster/\">[JMRI XML]</a></p>"
                "<table><tr><th>Slot</th><th>ID</th><th>DCC</th><th>Road</th>"
                "<th>Road#</th><th>Actions</th></tr>") != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    uint8_t count = roster_count_valid();
    while (p >= 3 && p < 3 + count) {
        uint8_t idx = (uint8_t)(p - 3);
        if (!roster_get_dense(idx, &http_conn.loco_slot, &http_conn.entry)) {
            http_conn.phase = ++p;
            continue;
        }
        const roster_entry_t *e = &http_conn.entry;
        snprintf(buf, sizeof(g_gen_buf),
            "<tr><td>%u</td><td>%s</td><td>%u%s</td><td>%s</td><td>%s</td>"
            "<td><a class=\"btn\" href=\"/edit?slot=%u\">[Edit]</a>"
            "<form class=\"inline\" method=\"POST\" action=\"/delete?slot=%u\" "
            "onsubmit=\"return confirm('Delete %s?');\">"
            "<button type=\"submit\">[Delete]</button></form></td></tr>",
            http_conn.loco_slot, e->id, e->dccAddress, e->longAddress ? "L" : "S",
            e->roadName, e->roadNumber,
            http_conn.loco_slot, http_conn.loco_slot, e->id);
        if (http_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    if (p == 3 + count) {
        if (http_write_str(pcb, "</table>") != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 4 + count) {
        if (http_write_str(pcb, kHtmlFoot) != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    conn_close(pcb);
    return true;
}

// ─── /edit form generator ────────────────────────────────────────────────
// Phase layout:
//   0    HTTP header + <html>...
//   1    <h1> + form open + hidden slot + fixed-field block
//   2+f  function row f (0..ROSTER_FUNC_MAX)
//   last submit + form close + </body>
static bool html_edit_send(struct tcp_pcb *pcb) {
    char *buf = g_gen_buf;
    uint16_t p = http_conn.phase;
    const roster_entry_t *e = &http_conn.entry;

    if (p == 0) {
        if (http_write_str(pcb,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Connection: close\r\n\r\n") != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 1) {
        if (http_write_str(pcb, kHtmlHead) != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 2) {
        const char *slot_val = (http_conn.arg_slot == SLOT_NEW) ? "new" : NULL;
        char slot_num[8]; slot_num[0] = '\0';
        if (!slot_val) { snprintf(slot_num, sizeof(slot_num), "%u", http_conn.arg_slot); slot_val = slot_num; }
        snprintf(buf, sizeof(g_gen_buf),
            "<h1>%s</h1>"
            "<p><a href=\"/roster.html\">[Back to roster]</a></p>"
            "<form method=\"POST\" action=\"/save\">"
            "<input type=\"hidden\" name=\"slot\" value=\"%s\">"
            "<table>"
            "<tr><th>ID</th><td><input name=\"id\" size=\"32\" maxlength=\"31\" value=\"%s\"></td></tr>"
            "<tr><th>Road name</th><td><input name=\"roadName\" size=\"32\" maxlength=\"31\" value=\"%s\"></td></tr>"
            "<tr><th>Road number</th><td><input name=\"roadNumber\" size=\"32\" maxlength=\"31\" value=\"%s\"></td></tr>",
            (http_conn.arg_slot == SLOT_NEW) ? "Add roster entry" : "Edit roster entry",
            slot_val, e->id, e->roadName, e->roadNumber);
        if (http_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    if (p == 3) {
        snprintf(buf, sizeof(g_gen_buf),
            "<tr><th>Manufacturer</th><td><input name=\"mfg\" size=\"32\" maxlength=\"31\" value=\"%s\"></td></tr>"
            "<tr><th>Model</th><td><input name=\"model\" size=\"32\" maxlength=\"31\" value=\"%s\"></td></tr>"
            "<tr><th>DCC address</th><td><input type=\"number\" name=\"dccAddress\" min=\"1\" max=\"10239\" value=\"%u\"></td></tr>"
            "<tr><th>Long address</th><td><input type=\"checkbox\" name=\"longAddress\" value=\"1\"%s></td></tr>"
            "<tr><th>Max speed</th><td><input type=\"number\" name=\"maxSpeed\" min=\"1\" max=\"255\" value=\"%u\"></td></tr>"
            "<tr><th>Max function #</th><td><input type=\"number\" name=\"maxFnNum\" min=\"0\" max=\"%u\" value=\"%u\"></td></tr>"
            "</table>"
            "<h2>Functions</h2>"
            "<table><tr><th>#</th><th>Label</th><th>Lockable</th><th>Visible</th></tr>",
            e->mfg, e->model, e->dccAddress,
            e->longAddress ? " checked" : "",
            e->maxSpeed, ROSTER_FUNC_MAX, e->maxFnNum);
        if (http_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    // Function rows: phases 4..(4 + ROSTER_FUNC_MAX)
    while (p >= 4 && p <= 4 + ROSTER_FUNC_MAX) {
        uint8_t fn = (uint8_t)(p - 4);
        snprintf(buf, sizeof(g_gen_buf),
            "<tr class=\"fn-row\"><td>F%u</td>"
            "<td><input name=\"f%u_label\" size=\"24\" maxlength=\"31\" value=\"%s\"></td>"
            "<td><input type=\"checkbox\" name=\"f%u_lockable\" value=\"1\"%s></td>"
            "<td><input type=\"checkbox\" name=\"f%u_visible\" value=\"1\"%s></td></tr>",
            fn,
            fn, e->functions[fn].label,
            fn, e->functions[fn].lockable ? " checked" : "",
            fn, e->functions[fn].visible ? " checked" : "");
        if (http_write_str(pcb, buf) != ERR_OK) return false;
        http_conn.phase = ++p;
    }

    if (p == 5 + ROSTER_FUNC_MAX) {
        if (http_write_str(pcb,
                "</table><p><button type=\"submit\">Save</button></p></form>") != ERR_OK)
            return false;
        http_conn.phase = ++p;
    }
    if (p == 6 + ROSTER_FUNC_MAX) {
        if (http_write_str(pcb, kHtmlFoot) != ERR_OK) return false;
        http_conn.phase = ++p;
    }
    conn_close(pcb);
    return true;
}

// ─── POST /save handler ──────────────────────────────────────────────────
static bool handle_save(struct tcp_pcb *pcb) {
    // Reuse http_conn.entry (already 1.2 KB in bss) instead of a stack
    // allocation, to keep the tcpip_thread stack usage low.
    roster_entry_t *entry = &http_conn.entry;
    memset(entry, 0, sizeof(*entry));

    // Parse the whole form body. slot=new or slot=N is one of the fields.
    // We handle it manually because it's not a roster_entry_t field.
    form_parse_roster(http_conn.post_buf, http_conn.post_len, entry);

    // Extract slot value from the body directly
    const char *slot_val = NULL;
    char slot_str[8]; slot_str[0] = '\0';
    {
        // linear search for "slot=" at start-of-body or after '&'
        const char *p = http_conn.post_buf;
        size_t remaining = http_conn.post_len;
        while (remaining > 5) {
            if ((p == http_conn.post_buf || *(p - 1) == '&') &&
                strncmp(p, "slot=", 5) == 0) {
                p += 5;
                size_t i = 0;
                while (i + 1 < sizeof(slot_str) &&
                       (size_t)(p - http_conn.post_buf) + i < http_conn.post_len &&
                       p[i] != '&' && p[i] != '\0') {
                    slot_str[i] = p[i];
                    i++;
                }
                slot_str[i] = '\0';
                slot_val = slot_str;
                break;
            }
            p++; remaining--;
        }
    }
    if (!slot_val) return send_400(pcb, "Missing slot field.\n");

    // Basic validation
    if (entry->id[0] == '\0') return send_400(pcb, "id must not be empty.\n");
    if (entry->dccAddress < 1 || entry->dccAddress > 10239)
        return send_400(pcb, "dccAddress out of range (1..10239).\n");
    if (entry->maxFnNum > ROSTER_FUNC_MAX)
        return send_400(pcb, "maxFnNum too large.\n");

    if (strcmp(slot_val, "new") == 0) {
        uint8_t new_slot;
        if (!roster_add(entry, &new_slot))
            return send_400(pcb, "roster_add failed (full or duplicate id).\n");
        LOG_INFO(TAG, "add: id='%s' slot=%u", entry->id, new_slot);
    } else {
        unsigned long n = strtoul(slot_val, NULL, 10);
        if (n >= ROSTER_MAX_ENTRIES) return send_400(pcb, "Invalid slot.\n");
        if (!roster_update((uint8_t)n, entry))
            return send_400(pcb, "roster_update failed (slot unused or duplicate id).\n");
        LOG_INFO(TAG, "update: id='%s' slot=%lu", entry->id, n);
    }
    return send_redirect(pcb, "/roster.html");
}

// ─── POST /delete handler ────────────────────────────────────────────────
static bool handle_delete(struct tcp_pcb *pcb) {
    if (http_conn.arg_slot >= ROSTER_MAX_ENTRIES)
        return send_400(pcb, "Missing or invalid slot.\n");
    if (!roster_delete((uint8_t)http_conn.arg_slot))
        return send_400(pcb, "roster_delete failed (slot unused).\n");
    LOG_INFO(TAG, "delete: slot=%u", http_conn.arg_slot);
    return send_redirect(pcb, "/roster.html");
}

// ─── Dispatch after headers (and body, for POST) are ready ───────────────
static void dispatch(struct tcp_pcb *pcb) {
    http_conn.state = HTTP_SENDING;
    http_conn.phase = 0;

    switch (http_conn.route) {
    case R_INDEX:
        send_redirect(pcb, "/roster.html");
        break;

    case R_ROSTER_XML:
        xml_send_chunk(pcb);
        break;

    case R_ROSTER_HTML:
        html_table_send(pcb);
        break;

    case R_EDIT:
        // Prime entry_scratch from the requested slot (or blank for "new")
        memset(&http_conn.entry, 0, sizeof(http_conn.entry));
        if (http_conn.arg_slot == SLOT_NEW) {
            // Blank form; provide a sensible maxFnNum default of 8
            http_conn.entry.maxSpeed = 100;
            http_conn.entry.maxFnNum = 8;
        } else if (http_conn.arg_slot < ROSTER_MAX_ENTRIES) {
            if (!roster_get((uint8_t)http_conn.arg_slot, &http_conn.entry)) {
                send_404(pcb);
                return;
            }
        } else {
            send_400(pcb, "Missing or invalid slot.\n");
            return;
        }
        html_edit_send(pcb);
        break;

    case R_SAVE:
        handle_save(pcb);
        break;

    case R_DELETE:
        handle_delete(pcb);
        break;

    case R_400:
        send_400(pcb, "Bad request.\n");
        break;

    default:
        send_404(pcb);
        break;
    }

    // Push any data buffered by the generator (partial run — will resume
    // on the next tcp_poll). If the generator finished, it already called
    // conn_close which flushes.
    if (http_conn.pcb == pcb && http_conn.state == HTTP_SENDING) {
        http_flush(pcb);
    }
}

// ─── lwIP callbacks ──────────────────────────────────────────────────────
static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        conn_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) { pbuf_free(p); return err; }

    // Acknowledge received bytes to lwIP FIRST — dispatch() may close the
    // connection, and tcp_recved on a closed pcb is undefined behavior.
    tcp_recved(pcb, p->tot_len);

    // Copy the incoming pbuf into a stack buffer for processing.
    static char rx[512];
    u16_t total_consumed = 0;
    bool closed = false;
    while (total_consumed < p->tot_len && !closed) {
        u16_t take = (p->tot_len - total_consumed < sizeof(rx))
                     ? (p->tot_len - total_consumed) : sizeof(rx);
        pbuf_copy_partial(p, rx, take, total_consumed);
        total_consumed += take;

        if (!http_conn.headers_done) {
            process_headers(rx, take);
            if (http_conn.headers_done) {
                // For POST, we may still need body bytes
                if (http_conn.method == M_POST &&
                    http_conn.post_len < http_conn.post_expected) {
                    http_conn.state = HTTP_RECV_BODY;
                } else {
                    dispatch(pcb);
                    closed = (http_conn.pcb == NULL);
                    break;
                }
            }
        } else if (http_conn.state == HTTP_RECV_BODY) {
            u16_t space = (http_conn.post_expected > http_conn.post_len)
                        ? (uint16_t)(http_conn.post_expected - http_conn.post_len) : 0;
            u16_t cp = (take < space) ? take : space;
            if (cp && http_conn.post_len + cp < POST_BUF_SIZE) {
                memcpy(http_conn.post_buf + http_conn.post_len, rx, cp);
                http_conn.post_len += cp;
            }
            if (http_conn.post_len >= http_conn.post_expected) {
                dispatch(pcb);
                closed = (http_conn.pcb == NULL);
                break;
            }
        }
    }

    pbuf_free(p);
    return ERR_OK;
}

static void http_err_cb(void *arg, err_t err) {
    (void)arg;
    // ERR_ABRT/ERR_RST/ERR_CLSD are normal peer-side close scenarios
    // (browser closed the tab, speculative connection abandoned, etc.).
    // Only log genuinely unexpected errors.
    if (err != ERR_ABRT && err != ERR_RST && err != ERR_CLSD) {
        LOG_WARN(TAG, "HTTP TCP error: %d", err);
    }
    conn_reset();
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb) {
    (void)arg;
    if (http_conn.pcb == pcb && http_conn.state == HTTP_SENDING) {
        // Continue whichever generator is running
        switch (http_conn.route) {
        case R_ROSTER_XML:   xml_send_chunk(pcb);   break;
        case R_ROSTER_HTML:  html_table_send(pcb);  break;
        case R_EDIT:         html_edit_send(pcb);   break;
        default: break;
        }
        // Push any buffered data out of lwIP now, in case the generator
        // finished all remaining phases without hitting ERR_MEM.
        if (http_conn.pcb == pcb) http_flush(pcb);
    }
    return ERR_OK;
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;

    if (http_conn.pcb != NULL) {
        LOG_WARN(TAG, "HTTP: rejecting second connection");
        tcp_close(newpcb);
        return ERR_OK;
    }

    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_nagle_disable(newpcb);       // ship small final segments immediately
    memset(&http_conn, 0, sizeof(http_conn));
    http_conn.pcb      = newpcb;
    http_conn.state    = HTTP_RECV_HEADERS;
    http_conn.arg_slot = SLOT_MISSING;

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv_cb);
    tcp_err(newpcb, http_err_cb);
    tcp_poll(newpcb, http_poll_cb, 1);   // 500 ms retry (was 1 s)

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
    LOG_INFO(TAG, "HTTP server listening on port %d", HTTP_ROSTER_PORT);
    return true;
}
