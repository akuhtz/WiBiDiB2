/*
 * bidib_client_if.c  —  WiBiDiB (Pico 2W)
 *
 * Portage depuis OpenDCC/Atmel (Wolfgang Kufer) → Pico bare-metal
 * Copyright (c) 2006-2011 Wolfgang Kufer — GNU GPL v2
 * Portage Pierre Moulin 2025
 *
 * Changements vs Atmel :
 *   - UART AVR (9 bits, ISR RXC/DRE/TXC) → PIO bidib_uart.pio (déjà écrit)
 *   - cli()/sei()             → save_and_disable_interrupts() / restore_interrupts()
 *   - ATOMIC_BLOCK            → idem
 *   - flag_reset/set()        → variables bool simples
 *   - cortos                  → supprimé
 *   - SET_BIDIB_TO_RX/TX()    → gpio_put(BIDIB_PIN_DE, 0/1)
 *   - _delay_us()             → busy_wait_us_32()
 *   - BIDIB_UART.DATA         → pio_sm_put/get()
 *
 * Architecture Pico :
 *   ISR RX (PIO0_IRQ_0) = bidib_pio_rx_isr() dans bidib.c
 *     → remplit bidib_rx_buf[] + détecte token logon
 *   ISR TX (PIO0_IRQ_1) = bidib_pio_tx_isr() dans bidib.c
 *     → envoie bidib_tx_buf[] octet par octet
 *   bidib_tx_fifo_put() → copie message dans bidib_tx_buf[]
 *   bidib_rx_read()     → lit bidib_rx_buf[]
 *
 * Note : les ISR PIO sont dans bidib.c — ce fichier gère uniquement
 *        les buffers et la logique de haut niveau.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "bidib.h"              // BIDIB_PIN_DE, MyUniqueID, bidib_state_t
#include "bidib_messages.h"
#include "bidib_distributed_control.h"
#include "bidib_client_if.h"
#include "crc_8bit.h"
#include "config.h"

static const char *TAG = "bidib_client";

extern uint8_t g_bidib_connect;
uint64_t last_poll_us = 0;
// ─── Buffers ─────────────────────────────────────────────────────────────────

// TX: circular buffer for messages to send
// Identical to Atmel — accessed from TX ISR (bidib_pio_tx_isr)
uint8_t          bidib_tx_buf[BIDIB_TX_BUF_SIZE];
volatile uint8_t bidib_tx_buf_read  = 0;
volatile uint8_t bidib_tx_buf_write = 0;
volatile uint8_t bidib_tx_fill      = 0;      // octets en attente (non encore chargés)
volatile uint8_t bidib_tx_remaining = 0;      // octets en cours d'envoi par ISR

#if (BIDIB_TX_BUF_USE_AHEAD == 1)
volatile uint8_t bidib_tx_ahead = 0;
#endif
volatile uint8_t bidib_tx_crc = 0;

// RX: FreeRTOS stream buffer for received words (uint16_t packed as 2 bytes)
// Written by bidib_pio_rx_isr(), read by BiDiB parser task
#define BIDIB_RX_STREAM_SIZE  256   // bytes (128 x uint16_t)
#define BIDIB_RX_TRIGGER      2     // wake on 2 bytes (1 x uint16_t)
static StreamBufferHandle_t bidib_rx_stream;

// TX hardware spinlock for bidib_tx_buf (shared between tasks and PIO TX ISR)
spin_lock_t *tx_spinlock;

// TX message sequence number
uint8_t bidib_tx0_msg_num = 0;

// ─── Direction RS485 ─────────────────────────────────────────────────────────
// Replaces SET_BIDIB_TO_RX/TX() Atmel

void set_bidib_to_receive(void) {
    gpio_put(BIDIB_PIN_DE, 0);   // DE=0 → RX  
    gpio_put(BIDIB_PIN_TEST , 1);
            busy_wait_us_32(8);
    gpio_put(BIDIB_PIN_TEST , 0);
}

void set_bidib_to_transmit(void) {
    gpio_put(BIDIB_PIN_DE, 1);   // DE=1 → TX  
}

// ─── RX buffer (FreeRTOS stream buffer) ────────────────────────────────────

bool bidib_rx_ready(void) {
    return xStreamBufferBytesAvailable(bidib_rx_stream) >= 2;
}

// Returns 16 bits: bits 0-7 = data, bit 8 = id_bit
uint16_t bidib_rx_read(void) {
    uint16_t word;
    xStreamBufferReceive(bidib_rx_stream, &word, 2, 0);
    return word;
}

// Called from bidib_pio_rx_isr() — ISR-safe stream buffer write
void bidib_rx_buf_put(uint16_t word) {
    xStreamBufferSendFromISR(bidib_rx_stream, &word, 2, NULL);
}

// ─── TX buffer state ─────────────────────────────────────────────────────────

bool bidib_tx_fifo_empty(void) {
    uint32_t saved = spin_lock_blocking(tx_spinlock);
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    spin_unlock(tx_spinlock, saved);
    return (filled < 16);
}

bool bidib_tx_fifo_ready(void) {
    uint32_t saved = spin_lock_blocking(tx_spinlock);
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    spin_unlock(tx_spinlock, saved);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_READY));
}

bool bidib_tx_fifo_okay(void) {
    uint32_t saved = spin_lock_blocking(tx_spinlock);
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    spin_unlock(tx_spinlock, saved);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_OKAY));
}

bool bidib_tx_fifo_healthy(void) {
    uint32_t saved = spin_lock_blocking(tx_spinlock);
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    spin_unlock(tx_spinlock, saved);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_HEALTHY));
}

// ─── bidib_tx_fifo_put() ─────────────────────────────────────────────────────
//
// Copies a message into the TX circular buffer.
// Identical to the Atmel version — calculates CRC and appends to end of message.
// Le message est : [size, node_addr, index, msg_type, data...]
// size = number of bytes following (without the size byte itself)
//
// Called by send_bidib_message() dans bidib_client_parser.c
//
bool bidib_tx_fifo_put(uint8_t *new_message) {
    if (g_bidib_connect != BIDIB_CONNECTED) return true;
 gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(2);
    gpio_put(BIDIB_PIN_TEST , 0);
    uint8_t size  = new_message[0];
    uint8_t total = size + 1;  // size + message

  #if (DEBUG == 1)  
    static uint8_t call_count = 0;
    call_count++;
    LOG_INFO(TAG,"[tx_put#%d] read=%d write=%d ahead=%d size=%d",
        call_count, bidib_tx_buf_read, bidib_tx_buf_write, bidib_tx_ahead, size);   
#endif
    uint32_t saved = spin_lock_blocking(tx_spinlock);
gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(2);
gpio_put(BIDIB_PIN_TEST , 0);     
    // Check available space
    if ((bidib_tx_ahead + total) > BIDIB_TX_BUF_SIZE) {
        spin_unlock(tx_spinlock, saved);
        LOG_INFO(TAG,"TX fifo full!");
        return false;
    }

    // Copy message into the fifo
    for (uint8_t i = 0; i <= size; i++) {
        bidib_tx_buf[bidib_tx_buf_write] = new_message[i];
        bidib_tx_buf_write = (bidib_tx_buf_write + 1) & (BIDIB_TX_BUF_SIZE - 1);
    }
     bidib_tx_ahead += total;
    
       spin_unlock(tx_spinlock, saved);
    return true;
}
// ─── Sequence Number ───────────────────────────────────────────────────────
// Identical to get_tx_num() Atmel

uint8_t bidib_get_tx_num(void) {
    uint8_t retval = bidib_tx0_msg_num++;
    if (bidib_tx0_msg_num == 0) bidib_tx0_msg_num = 1;
    return retval;
}

// ─── Logon message dans TX buf ───────────────────────────────────────────────
//
// Prepares the MSG_LOGON message directly in bidib_tx_buf[]
// (not via fifo_put — logon is special, sent before connection is established)
// Identical to bidib_copy_logon_to_txbuf() Atmel
// + bidib_prepare_logon_buf() already in bidib.c — we unify here
//
void bidib_prepare_tx_logon(void) {
    // Format: [size=10][addr=0][mnum=0][MSG_LOGON][UID x7]
    // CRC is calculated and added by l'ISR TX (like Atmel)
    // or here if we want to pre-calculate
    uint8_t idx = 0;
    bidib_tx_buf[idx++] = BIDIB_SIZE_OF_LOGON_MSG - 1;  // size = 10
    bidib_tx_buf[idx++] = 0x00;                          // addr = 0
    bidib_tx_buf[idx++] = 0x00;                          // mnum = 0
    bidib_tx_buf[idx++] = MSG_LOGON;
    for (int i = 0; i < 7; i++)
        bidib_tx_buf[idx++] = MyUniqueID[i];

    // CRC on the whole message
    uint8_t crc = 0;
    for (int i = 0; i < BIDIB_SIZE_OF_LOGON_MSG; i++)
        crc = crc8_update(crc, bidib_tx_buf[i]);
    bidib_tx_buf[idx++] = crc;
#if (DEBUG == 1)
    LOG_INFO(TAG,"Logon prepared, crc=0x%02X, total=%d bytes",crc, idx);)
#endif
}

// ─── Flush buffers ────────────────────────────────────────────────────────────

void bidib_flush_rx(void) {
    xStreamBufferReset(bidib_rx_stream);
}

void bidib_flush_tx(void) {
    uint32_t saved = spin_lock_blocking(tx_spinlock);
    bidib_tx_buf_read  = 0;
    bidib_tx_buf_write = BIDIB_SIZE_OF_LOGON_MSG + 1;  // réservé pour logon
    bidib_tx_remaining = 0;
    bidib_tx_fill      = 0;
    bidib_tx_ahead     = 0;
    spin_unlock(tx_spinlock, saved);
    bidib_prepare_tx_logon();
}

// ─── Init ─────────────────────────────────────────────────────────────────────
//
// Equivalent to init_bidib_client_if() Atmel
// The PIO (9-bit UART) is already initialized in bidib_init() (bidib.c)
// Here we initialize only the buffers and state

// Phase 1: Create stream buffer + spinlock (call BEFORE bidib_init)
void init_bidib_client_if_buffers(void) {
    bidib_rx_stream = xStreamBufferCreate(BIDIB_RX_STREAM_SIZE, BIDIB_RX_TRIGGER);
    tx_spinlock = spin_lock_init(next_striped_spin_lock_num());
    LOG_INFO(TAG,"buffers init done (stream buffer + spinlock)");
}

// Phase 2: Init buffer state + direction (call AFTER bidib_init)
void init_bidib_client_if(void) {
    set_bidib_to_receive();
    bidib_flush_rx();
    bidib_flush_tx();
    my_bidib_node_addr = 0xFF;
    bidib_tx0_msg_num  = 1;
    LOG_INFO(TAG,"init done");
}

void stop_bidib_client_if(void) {
    set_bidib_to_receive();
    bidib_flush_rx();
    bidib_flush_tx();
}
