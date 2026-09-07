/*
 * roster.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Hardcoded sample roster for EngineDriver.
 * Replace with flash-stored data for production use.
 */
#include <string.h>
#include "roster.h"

static const char *TAG = "roster";

roster_t roster;

static void add_entry(const char *id, const char *roadName, const char *roadNumber,
                      const char *mfg, const char *model,
                      const char *decoderModel, const char *decoderFamily,
                      uint16_t dccAddr, bool longAddr, uint8_t maxSpeed,
                      uint8_t maxFnNum) {
    if (roster.count >= ROSTER_MAX_ENTRIES) return;
    roster_entry_t *e = &roster.entries[roster.count];
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, ROSTER_STRING_MAX - 1);
    strncpy(e->roadName, roadName, ROSTER_STRING_MAX - 1);
    strncpy(e->roadNumber, roadNumber, ROSTER_STRING_MAX - 1);
    strncpy(e->mfg, mfg, ROSTER_STRING_MAX - 1);
    strncpy(e->model, model, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderModel, decoderModel, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderFamily, decoderFamily, ROSTER_STRING_MAX - 1);
    e->dccAddress   = dccAddr;
    e->longAddress  = longAddr;
    e->maxSpeed     = maxSpeed;
    e->maxFnNum     = maxFnNum;
    roster.count++;
}

static void set_func(uint8_t locoIndex, uint8_t fn, bool lockable,
                     bool visible, const char *label) {
    if (locoIndex >= roster.count) return;
    if (fn > ROSTER_FUNC_MAX) return;
    roster_entry_t *e = &roster.entries[locoIndex];
    e->functions[fn].lockable = lockable;
    e->functions[fn].visible  = visible;
    strncpy(e->functions[fn].label, label, ROSTER_STRING_MAX - 1);
}

void roster_init(void) {
    memset(&roster, 0, sizeof(roster));

    // ── Loco 0: ESU LokSound 5 diesel ────────────────────────────────────
    add_entry("BR 185 203-1", "DB", "185 203-1", "TRIX", "TRAXX F140 AC",
              "ESU LokSound 5", "ESU LokSound 5", 3, false, 100, 6);
    set_func(0, 0,  true,  true,  "Headlight");
    set_func(0, 1,  false, true,  "Bell");
    set_func(0, 2,  false, true,  "Horn");
    set_func(0, 3,  true, true,  "Compressor");
    set_func(0, 4,  true, true,  "Cab Light");
    set_func(0, 5,  true, true,  "Ditch Lights");

    // ── Loco 1: LokSound 5 steam ─────────────────────────────────────────
    add_entry("BR 01 1066-9", "DB", "01 1066-9", "ROCO", "BR 01",
              "ESU LokSound 5", "ESU LokSound 5", 1, false, 100, 8);
    set_func(1, 0,  true,  true,  "Headlight");
    set_func(1, 1,  false, true,  "Bell");
    set_func(1, 2,  false, true,  "Whistle");
    set_func(1, 3,  true, true,  "Steam Blowoff");
    set_func(1, 4,  true, true,  "Safety Valve");
    set_func(1, 5,  true, true,  "Oil Cup");
    set_func(1, 6,  true, true,  "Generator");
    set_func(1, 7,  true, true,  "Cab Light");

    // ── Loco 2: NCE decoder, US diesel ────────────────────────────────────
    add_entry("UP 4014", "Union Pacific", "4014", "Athearn", "DD40AX Big Boy",
              "NCE DCC Gold", "NCE", 4014, true, 100, 6);
    set_func(2, 0,  true,  true,  "Headlight");
    set_func(2, 1,  true, true,  "Bell");
    set_func(2, 2,  false, true,  "Horn");
    set_func(2, 3,  true, true,  "Ditch Lights");
    set_func(2, 4,  true, true,  "Cab Light");
    set_func(2, 5,  true, true,  "Dynamic Brakes");
}

const roster_entry_t* roster_get(uint8_t index) {
    if (index >= roster.count) return NULL;
    return &roster.entries[index];
}

int roster_find_by_id(const char* id) {
    for (uint8_t i = 0; i < roster.count; i++) {
        if (strcmp(roster.entries[i].id, id) == 0) return i;
    }
    return -1;
}
