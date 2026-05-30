/*
 * bidib_client_if.h  —  WiBiDiB (Pico 2W)
 *
 * Portage depuis OpenDCC/Atmel (Wolfgang Kufer) → Pico bare-metal
 * Pierre Moulin 2025
 */

#ifndef BIDIB_CLIENT_IF_H_
#define BIDIB_CLIENT_IF_H_

#include <stdint.h>
#include <stdbool.h>
#include "bidib.h"          // bidib_state_t, BIDIB_PIN_DE, MyUniqueID
#include "bidib_messages.h"

// ─── Tailles buffers ──────────────────────────────────────────────────────────
#define BIDIB_TX_BUF_SIZE           128     // DOIT être puissance de 2
#define BIDIB_TX_BUF_REST_READY      16
#define BIDIB_TX_BUF_REST_HEALTHY    40
#define BIDIB_TX_BUF_REST_OKAY       60
#define BIDIB_RX_BUF_SIZE            64

#if (BIDIB_TX_BUF_SIZE > 64)
    #define BIDIB_TX_BUF_USE_AHEAD   1
#else
    #define BIDIB_TX_BUF_USE_AHEAD   0
#endif

// ─── Taille message LOGON ────────────────────────────────────────────────────
// [size=10][addr=0][mnum=0][MSG_LOGON][UID x7] = 11 octets + CRC = 12
#define BIDIB_SIZE_OF_LOGON_MSG     11      // sans CRC

// ─── Structure message (flat addressing) ─────────────────────────────────────
// Identique Atmel — utilisé par bidib_client_parser.c
typedef struct {
    uint8_t size;       // nb octets qui suivent (sans size)
    uint8_t node_addr;  // adresse du nœud destinataire
    uint8_t index;      // numéro de séquence
    uint8_t msg_type;   // type de message BiDiB
} t_node_message_header;

typedef struct {
    t_node_message_header header;
    uint8_t data;
} t_node_message1;

typedef struct {
    t_node_message_header header;
    uint8_t data[2];
} t_node_message2;

typedef struct {
    t_node_message_header header;
    uint8_t data[10];
} t_node_message10;

typedef struct {
    t_node_message_header header;
    uint8_t data[18];
} t_node_message18;

typedef struct {
    t_node_message_header header;
    uint8_t data[28];
} t_node_message28;

// ─── Variables globales ───────────────────────────────────────────────────────
extern uint8_t           my_bidib_node_addr;
extern uint8_t           g_bidib_backoff;

extern uint8_t           bidib_tx_buf[BIDIB_TX_BUF_SIZE];
extern volatile uint8_t  bidib_tx_buf_read;
extern volatile uint8_t  bidib_tx_buf_write;
extern volatile uint8_t  bidib_tx_fill;
extern volatile uint8_t  bidib_tx_remaining;
#if (BIDIB_TX_BUF_USE_AHEAD == 1)
extern uint8_t           bidib_tx_ahead;
#endif

extern uint16_t bidib_rx_buf[BIDIB_RX_BUF_SIZE];
extern uint8_t  bidib_rx_buf_read;
extern uint8_t  bidib_rx_buf_write;
extern uint8_t  bidib_rx_fill;

// ─── API publique ─────────────────────────────────────────────────────────────
void     init_bidib_client_if(void);
void     stop_bidib_client_if(void);

void     set_bidib_to_receive(void);
void     set_bidib_to_transmit(void);

void     bidib_flush_rx(void);
void     bidib_flush_tx(void);

// TX
bool     bidib_tx_fifo_put(uint8_t *new_message);
bool     bidib_tx_fifo_empty(void);
bool     bidib_tx_fifo_ready(void);
bool     bidib_tx_fifo_okay(void);
bool     bidib_tx_fifo_healthy(void);
uint8_t  bidib_get_tx_num(void);

// RX
bool     bidib_rx_ready(void);
uint16_t bidib_rx_read(void);
void     bidib_rx_buf_put(uint16_t word);   // appelé depuis bidib_pio_rx_isr()

// Logon
void     bidib_prepare_tx_logon(void);

#endif /* BIDIB_CLIENT_IF_H_ */
