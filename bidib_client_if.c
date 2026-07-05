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
#include "pico/sync.h"          // save_and_disable_interrupts / restore_interrupts
#include "hardware/pio.h"
#include "hardware/gpio.h"

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

// TX : buffer circulaire pour les messages à envoyer
// Identique Atmel — accès depuis ISR TX (bidib_pio_tx_isr)
uint8_t          bidib_tx_buf[BIDIB_TX_BUF_SIZE];
volatile uint8_t bidib_tx_buf_read  = 0;
volatile uint8_t bidib_tx_buf_write = 0;
volatile uint8_t bidib_tx_fill      = 0;      // octets en attente (non encore chargés)
volatile uint8_t bidib_tx_remaining = 0;      // octets en cours d'envoi par ISR

#if (BIDIB_TX_BUF_USE_AHEAD == 1)
volatile uint8_t bidib_tx_ahead = 0;
#endif
volatile uint8_t bidib_tx_crc = 0;

// RX : buffer circulaire pour les octets reçus (16 bits : octet + id_bit)
// Rempli par bidib_pio_rx_isr() dans bidib.c
uint16_t bidib_rx_buf[BIDIB_RX_BUF_SIZE];
uint8_t  bidib_rx_buf_read  = 0;
uint8_t  bidib_rx_buf_write = 0;
uint8_t  bidib_rx_fill      = 0;

// Numéro de séquence des messages TX
uint8_t bidib_tx0_msg_num = 0;

// ─── Direction RS485 ─────────────────────────────────────────────────────────
// Remplace SET_BIDIB_TO_RX/TX() Atmel

void set_bidib_to_receive(void) {
    gpio_put(BIDIB_PIN_DE, 0);   // DE=0 → RX  
    gpio_put(BIDIB_PIN_TEST , 1);
            busy_wait_us_32(8);
    gpio_put(BIDIB_PIN_TEST , 0);
}

void set_bidib_to_transmit(void) {
    gpio_put(BIDIB_PIN_DE, 1);   // DE=1 → TX  
}

// ─── RX buffer ───────────────────────────────────────────────────────────────

bool bidib_rx_ready(void) {
    return (bidib_rx_buf_read != bidib_rx_buf_write);
}

// Retourne 16 bits : bits 0-7 = data, bit 8 = id_bit
// Identique à bidib_rx_read() Atmel
uint16_t bidib_rx_read(void) {
    uint16_t retval = bidib_rx_buf[bidib_rx_buf_read];
    bidib_rx_buf_read++;
    if (bidib_rx_buf_read == BIDIB_RX_BUF_SIZE) bidib_rx_buf_read = 0;
    return retval;
}

// Appelé depuis bidib_pio_rx_isr() pour écrire dans le buffer RX
// (remplace l'ISR RXC Atmel qui écrivait directement)
void bidib_rx_buf_put(uint16_t word) {
    uint8_t next_write = (bidib_rx_buf_write + 1) % BIDIB_RX_BUF_SIZE;
    if (next_write != bidib_rx_buf_read) {  // pas plein
        bidib_rx_buf[bidib_rx_buf_write] = word;
        bidib_rx_buf_write = next_write;
        bidib_rx_fill++;
    }
    // sinon : overflow silencieux (comme Atmel "no overrun check")
}

// ─── TX buffer state ─────────────────────────────────────────────────────────

bool bidib_tx_fifo_empty(void) {
    uint32_t s = bidib_enter_critical();
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    bidib_exit_critical(s);
    return (filled < 16);
}

bool bidib_tx_fifo_ready(void) {
    uint32_t s = bidib_enter_critical();
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    bidib_exit_critical(s);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_READY));
}

bool bidib_tx_fifo_okay(void) {
    uint32_t s = bidib_enter_critical();
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    bidib_exit_critical(s);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_OKAY));
}

bool bidib_tx_fifo_healthy(void) {
    uint32_t s = bidib_enter_critical();
    uint8_t filled = bidib_tx_fill + bidib_tx_remaining;
    bidib_exit_critical(s);
    return (filled <= (BIDIB_TX_BUF_SIZE - BIDIB_TX_BUF_REST_HEALTHY));
}

// ─── bidib_tx_fifo_put() ─────────────────────────────────────────────────────
//
// Copie un message dans le buffer TX circulaire.
// Identique à la version Atmel — calcule le CRC et ajoute en fin de message.
// Le message est : [size, node_addr, index, msg_type, data...]
// size = nombre d'octets qui suivent (sans le byte size lui-même)
//
// Appelé par send_bidib_message() dans bidib_client_parser.c
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
    uint32_t s = bidib_enter_critical();
gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(2);
gpio_put(BIDIB_PIN_TEST , 0);     
    // Vérifier place disponible
    if ((bidib_tx_ahead + total) > BIDIB_TX_BUF_SIZE) {
        bidib_exit_critical(s);
        LOG_INFO(TAG,"TX fifo full!");
        return false;
    }

    // Copier message dans le fifo
    for (uint8_t i = 0; i <= size; i++) {
        bidib_tx_buf[bidib_tx_buf_write] = new_message[i];
        bidib_tx_buf_write = (bidib_tx_buf_write + 1) & (BIDIB_TX_BUF_SIZE - 1);
    }
     bidib_tx_ahead += total;  // une seule fois
    
       bidib_exit_critical(s);
    return true;
}
// ─── Numéro de séquence ───────────────────────────────────────────────────────
// Identique get_tx_num() Atmel

uint8_t bidib_get_tx_num(void) {
    uint8_t retval = bidib_tx0_msg_num++;
    if (bidib_tx0_msg_num == 0) bidib_tx0_msg_num = 1;
    return retval;
}

// ─── Logon message dans TX buf ───────────────────────────────────────────────
//
// Prépare le message MSG_LOGON directement dans bidib_tx_buf[]
// (pas via fifo_put — le logon est spécial, envoyé avant connexion établie)
// Identique à bidib_copy_logon_to_txbuf() Atmel
// + bidib_prepare_logon_buf() déjà dans bidib.c — on unifie ici
//
void bidib_prepare_tx_logon(void) {
    // Format : [size=10][addr=0][mnum=0][MSG_LOGON][UID x7]
    // La CRC est calculée et ajoutée par l'ISR TX (comme Atmel)
    // ou ici si on veut pré-calculer
    uint8_t idx = 0;
    bidib_tx_buf[idx++] = BIDIB_SIZE_OF_LOGON_MSG - 1;  // size = 10
    bidib_tx_buf[idx++] = 0x00;                          // addr = 0
    bidib_tx_buf[idx++] = 0x00;                          // mnum = 0
    bidib_tx_buf[idx++] = MSG_LOGON;
    for (int i = 0; i < 7; i++)
        bidib_tx_buf[idx++] = MyUniqueID[i];

    // CRC sur tout le message
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
    uint32_t s = bidib_enter_critical();
    bidib_rx_buf_read  = 0;
    bidib_rx_buf_write = 0;
    bidib_rx_fill      = 0;
    bidib_exit_critical(s);
}

void bidib_flush_tx(void) {
    uint32_t s = bidib_enter_critical();
    bidib_tx_buf_read  = 0;
    bidib_tx_buf_write = BIDIB_SIZE_OF_LOGON_MSG + 1;  // réservé pour logon
    bidib_tx_remaining = 0;
    bidib_tx_fill      = 0;
    bidib_tx_ahead     = 0;
    bidib_exit_critical(s);
    bidib_prepare_tx_logon();
}

// ─── Init ─────────────────────────────────────────────────────────────────────
//
// Équivalent init_bidib_client_if() Atmel
// Le PIO (UART 9 bits) est déjà initialisé dans bidib_init() (bidib.c)
// Ici on initialise uniquement les buffers et l'état
//
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
