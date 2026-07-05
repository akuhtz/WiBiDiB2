#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "pico/critical_section.h"

static inline uint32_t bidib_enter_critical(void) {
    return save_and_disable_interrupts();
}
static inline void bidib_exit_critical(uint32_t state) {
    restore_interrupts(state);
}

static inline uint32_t get_tick_ms(void) {
    return (uint32_t)(time_us_64() / 1000ULL);
}

extern uint64_t last_poll_us;
extern volatile uint8_t g_bidib_guest_enabled;

// ─────────────────────────────────────────────────
// Pins
// ─────────────────────────────────────────────────
#define BIDIB_PIN_RX    19   // gp19  pin 25
#define BIDIB_PIN_TX    18   // gp18  pin 24
#define BIDIB_PIN_DE     6   // gp6   pin 9
#define BIDIB_PIN_TEST  16   // gp16  pin 21                                                                                        

// ─────────────────────────────────────────────────
// Timing
// ─────────────────────────────────────────────────
#define BIDIB_BAUD          500000
#define BIDIB_CA_TIME_US    17

// ─────────────────────────────────────────────────
// Tokens BiDiBus
// ─────────────────────────────────────────────────
#define BIDIBUS_LOGON       0x7E

// ─────────────────────────────────────────────────
// UID
// ─────────────────────────────────────────────────
#define BIDIB_UID_CLASS     0x00
#define BIDIB_UID_CLASSX    0x01
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

extern volatile bool tx_mode_logon;
extern uint8_t my_addr_stack[4];
extern uint8_t my_addr_depth;
extern uint8_t bidib_tx0_msg_num;

extern volatile uint8_t bidib_tx_buf_write;
extern volatile uint8_t bidib_tx_buf_read;
extern volatile uint8_t bidib_tx_ahead;

// ─────────────────────────────────────────────────
// Distributed control
// ─────────────────────────────────────────────────
// ─────────────────────────────────────────────────
// Distributed control
// ─────────────────────────────────────────────────
#define BIDIB_TARGET_MODE_DCCGEN        0x0C
#define BIDIB_TARGET_MODE_BOOSTER       0x09

// Bitfield SUBSCRIPTION 16 bits (downstream)
#define SUBSCRIPTION_TRACK_SIGNAL       0x0040  // bit 6 = Track signal (DCC)
#define SUBSCRIPTION_BOOSTER            0x0008  // bit 3 = Booster messages
#define SUBSCRIPTION_ALL                0xFFFF

#define SUBSCRIPTION_ACK_OK             0x00
#define SUBSCRIPTION_ACK_CHANGED        0x01

// ─────────────────────────────────────────────────
// API publique
// ─────────────────────────────────────────────────
void    bidib_init(void);

