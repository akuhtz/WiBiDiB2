/*
 * roster.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Roster data model for EngineDriver HTTP access.
 * Stores locomotive entries with function labels.
 * Initially hardcoded, later movable to flash.
 */
#ifndef ROSTER_H_
#define ROSTER_H_

#include <stdint.h>
#include <stdbool.h>

#define ROSTER_MAX_ENTRIES    32
#define ROSTER_FUNC_MAX       28
#define ROSTER_STRING_MAX     32

typedef struct {
    char     id[ROSTER_STRING_MAX];
    char     roadName[ROSTER_STRING_MAX];
    char     roadNumber[ROSTER_STRING_MAX];
    char     mfg[ROSTER_STRING_MAX];
    char     model[ROSTER_STRING_MAX];
    char     decoderModel[ROSTER_STRING_MAX];
    char     decoderFamily[ROSTER_STRING_MAX];
    uint16_t dccAddress;
    bool     longAddress;
    uint8_t  maxSpeed;
    uint8_t  maxFnNum;
    struct {
        bool  lockable;
        bool  visible;
        char  label[ROSTER_STRING_MAX];
    } functions[ROSTER_FUNC_MAX + 1];
} roster_entry_t;

typedef struct {
    roster_entry_t entries[ROSTER_MAX_ENTRIES];
    uint8_t        count;
} roster_t;

extern roster_t roster;

void roster_init(void);
const roster_entry_t* roster_get(uint8_t index);
int roster_find_by_id(const char* id);

#endif /* ROSTER_H_ */
