/*
 * form_parser.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * URL-encoded form parser: key=value&key=value.
 * Handles %XX hex escapes and '+' → space.
 *
 * Function fields are named f0_label, f0_lockable, f0_visible, f1_label, ...
 * They are matched by prefix; the number is parsed and clamped to
 * [0, ROSTER_FUNC_MAX].
 */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "form_parser.h"

typedef enum { FT_STR, FT_UINT, FT_BOOL } field_type_t;

typedef struct {
    const char  *key;
    field_type_t type;
    size_t       offset;   // offset into roster_entry_t
    size_t       max_len;  // for FT_STR: char array size (incl. NUL)
} field_desc_t;

static const field_desc_t g_fields[] = {
    { "id",            FT_STR,  offsetof(roster_entry_t, id),            ROSTER_STRING_MAX },
    { "roadName",      FT_STR,  offsetof(roster_entry_t, roadName),      ROSTER_STRING_MAX },
    { "roadNumber",    FT_STR,  offsetof(roster_entry_t, roadNumber),    ROSTER_STRING_MAX },
    { "mfg",           FT_STR,  offsetof(roster_entry_t, mfg),           ROSTER_STRING_MAX },
    { "model",         FT_STR,  offsetof(roster_entry_t, model),         ROSTER_STRING_MAX },
    { "dccAddress",    FT_UINT, offsetof(roster_entry_t, dccAddress),    sizeof(uint16_t)  },
    { "longAddress",   FT_BOOL, offsetof(roster_entry_t, longAddress),   sizeof(bool)      },
    { "maxSpeed",      FT_UINT, offsetof(roster_entry_t, maxSpeed),      sizeof(uint8_t)   },
    { "maxFnNum",      FT_UINT, offsetof(roster_entry_t, maxFnNum),      sizeof(uint8_t)   },
};
#define N_FIELDS (sizeof(g_fields) / sizeof(g_fields[0]))

// ─── URL decoding: writes decoded bytes into out[], NUL-terminates ────────
// Reads at most `in_len` chars from `in`. Writes at most `out_size-1` bytes.
// Returns the number of decoded bytes written.
static size_t url_decode(const char *in, size_t in_len, char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        char c = in[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < in_len) {
            char hi = in[i + 1], lo = in[i + 2];
            int v = 0;
            if      (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
            else if (hi >= 'a' && hi <= 'f') v = (hi - 'a' + 10) << 4;
            else if (hi >= 'A' && hi <= 'F') v = (hi - 'A' + 10) << 4;
            if      (lo >= '0' && lo <= '9') v |= (lo - '0');
            else if (lo >= 'a' && lo <= 'f') v |= (lo - 'a' + 10);
            else if (lo >= 'A' && lo <= 'F') v |= (lo - 'A' + 10);
            out[o++] = (char)v;
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

// ─── Apply one key=value pair to *out ────────────────────────────────────
static void apply_pair(const char *key, const char *val, roster_entry_t *out) {
    // First check function fields: f<N>_(label|lockable|visible)
    if (key[0] == 'f' && key[1] >= '0' && key[1] <= '9') {
        // Parse the number
        int fn = 0;
        const char *p = key + 1;
        while (*p >= '0' && *p <= '9') { fn = fn * 10 + (*p - '0'); p++; }
        if (fn < 0 || fn > ROSTER_FUNC_MAX) return;
        if (*p != '_') return;
        p++;
        if (strcmp(p, "label") == 0) {
            strncpy(out->functions[fn].label, val, ROSTER_STRING_MAX - 1);
            out->functions[fn].label[ROSTER_STRING_MAX - 1] = '\0';
        } else if (strcmp(p, "lockable") == 0) {
            out->functions[fn].lockable = (val[0] != '\0' && val[0] != '0');
        } else if (strcmp(p, "visible") == 0) {
            out->functions[fn].visible = (val[0] != '\0' && val[0] != '0');
        }
        return;
    }

    // Regular fields
    for (size_t i = 0; i < N_FIELDS; i++) {
        if (strcmp(key, g_fields[i].key) != 0) continue;
        void *addr = (uint8_t *)out + g_fields[i].offset;
        switch (g_fields[i].type) {
        case FT_STR:
            strncpy((char *)addr, val, g_fields[i].max_len - 1);
            ((char *)addr)[g_fields[i].max_len - 1] = '\0';
            break;
        case FT_UINT: {
            unsigned long v = strtoul(val, NULL, 10);
            if (g_fields[i].max_len == sizeof(uint16_t)) {
                *(uint16_t *)addr = (uint16_t)v;
            } else {
                *(uint8_t *)addr = (uint8_t)v;
            }
            break;
        }
        case FT_BOOL:
            *(bool *)addr = (val[0] != '\0' && val[0] != '0');
            break;
        }
        return;
    }
}

// ─── Public entry ────────────────────────────────────────────────────────
bool form_parse_roster(const char *body, size_t len, roster_entry_t *out) {
    char key[24];
    char val[ROSTER_STRING_MAX];

    size_t i = 0;
    while (i < len) {
        // key
        size_t k_start = i;
        while (i < len && body[i] != '=' && body[i] != '&') i++;
        size_t k_end = i;
        url_decode(body + k_start, k_end - k_start, key, sizeof(key));

        // value (if any)
        val[0] = '\0';
        if (i < len && body[i] == '=') {
            i++;
            size_t v_start = i;
            while (i < len && body[i] != '&') i++;
            url_decode(body + v_start, i - v_start, val, sizeof(val));
        }
        // skip '&'
        if (i < len && body[i] == '&') i++;

        if (key[0] != '\0') apply_pair(key, val, out);
    }
    return true;
}
