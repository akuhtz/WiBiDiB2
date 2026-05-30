#pragma once

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────────
// Pins
// ─────────────────────────────────────────────────
#define BIDIB_PIN_RX    19   // gp19  pin 25
#define BIDIB_PIN_TX    18   // gp18  pin 24
#define BIDIB_PIN_DE     4   // gp4   pin 6
#define BIDIB_PIN_TEST  16   // gp16  pin 21

// ─────────────────────────────────────────────────
// Timing
// ─────────────────────────────────────────────────
#define BIDIB_BAUD          500000.0f
#define BIDIB_CA_TIME_US    17

// ─────────────────────────────────────────────────
// Tokens BiDiBus
// ─────────────────────────────────────────────────
#define BIDIBUS_LOGON       0x7E

// ─────────────────────────────────────────────────
// UID
// ─────────────────────────────────────────────────
#define BIDIB_UID_CLASS     0x80
#define BIDIB_UID_CLASSX    0x00
#define BIDIB_UID_VID       0x13
extern const uint8_t MyUniqueID[7];

// ─────────────────────────────────────────────────
// État de connexion
// ─────────────────────────────────────────────────
typedef enum {
    BIDIB_DISCONNECTED = 0,
    BIDIB_APPLIED,
    BIDIB_CONNECTED,
    BIDIB_REJECTED,
} bidib_state_t;


// ─────────────────────────────────────────────────
// API publique
// ─────────────────────────────────────────────────
void    bidib_init(void);

