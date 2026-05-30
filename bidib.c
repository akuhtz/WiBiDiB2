/*
 * bidib.c — BIDIBrc Pico 2W
 *
 * Architecture :
 * - ISR RX : détection token, collision avoidance, démarrage TX non bloquant
 * - ISR TX : envoi octet par octet depuis buffer (comme DRE interrupt ATMEL)
 */

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include "bidib_uart.pio.h"
#include "bidib.h"
#include "bidib_client_if.h"
#include "crc_8bit.h"

// ─────────────────────────────────────────────────
// UID unique du nœud
// ─────────────────────────────────────────────────
const uint8_t MyUniqueID[7] = {
    BIDIB_UID_CLASS,
    BIDIB_UID_CLASSX,
   
    0x13, 0xBA, 0xF1, 0xB6, 0xBC
};

// ─────────────────────────────────────────────────
// Variables globales
// ─────────────────────────────────────────────────
static PIO  s_pio   = pio0;
static uint s_sm_rx = 0;
static uint s_sm_tx = 1;
extern uint8_t g_bidib_connect;
extern uint8_t g_bidib_backoff;
extern uint8_t my_bidib_node_addr;

// ─────────────────────────────────────────────────
// Buffer TX — comme ATMEL
// ─────────────────────────────────────────────────
#define TX_BUF_SIZE 32
static volatile uint16_t tx_buf[TX_BUF_SIZE];
static volatile uint8_t  tx_remaining = 0;
static volatile uint8_t  tx_index     = 0;

// ─────────────────────────────────────────────────
// Préparer le buffer TX avec le message LOGON
// ─────────────────────────────────────────────────
static void bidib_prepare_logon_buf(void)
{
    uint8_t plength = 0x0B;
    uint8_t msg[11];
    msg[0] = 0x0A;
    msg[1] = 0x00;
    msg[2] = 0x00;
    msg[3] = MSG_LOCAL_LOGON;
    for (int i = 0; i < 7; i++)
        msg[4 + i] = MyUniqueID[i];

    uint8_t crc = 0;
    crc = crc8_update(crc, plength);
    for (int i = 0; i < 11; i++)
        crc = crc8_update(crc, msg[i]);

    // Remplir tx_buf — tous id_bit=0
    uint8_t idx = 0;
    tx_buf[idx++] = plength;
    for (int i = 0; i < 11; i++)
        tx_buf[idx++] = msg[i];
    tx_buf[idx++] = crc;

    printf("logon buf: plength=0x%02X crc=0x%02X\n", plength, crc);
    
}

#define BIDIB_LOGON_MSG_SIZE 13

// ─────────────────────────────────────────────────
// ISR TX — envoie les octets suivants depuis tx_buf
// ─────────────────────────────────────────────────
static void __not_in_flash_func(bidib_pio_tx_isr)(void)
{
    pio_interrupt_clear(s_pio, 1);

    if (tx_remaining > 0) {
        uint16_t w = tx_buf[tx_index++];
        tx_remaining--;
        pio_sm_put(s_pio, s_sm_tx, (uint32_t)w);
    } else {
        // Fin de transmission
        gpio_put(BIDIB_PIN_DE, 0);
        irq_set_enabled(PIO0_IRQ_1, false);
    }
}

// ─────────────────────────────────────────────────
// Démarrer TX — non bloquant, comme ATMEL
// ─────────────────────────────────────────────────
static void __not_in_flash_func(bidib_start_tx)(void)
{
    tx_index     = 0;
    tx_remaining = BIDIB_LOGON_MSG_SIZE - 1;  // -1 : premier octet envoyé ici

    gpio_put(BIDIB_PIN_DE, 1);

    // Premier octet — comme ATMEL UART.DATA = first
    pio_sm_put(s_pio, s_sm_tx, (uint32_t)tx_buf[tx_index++]);

    // Activer ISR TX pour les suivants
    irq_set_enabled(PIO0_IRQ_1, true);
}

// ─────────────────────────────────────────────────
// ISR RX — version qui fonctionnait + bidib_start_tx()
// ─────────────────────────────────────────────────
static void __not_in_flash_func(bidib_pio_rx_isr)(void)
{
    pio_interrupt_clear(s_pio, 0);

    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) {

        uint32_t raw    = pio_sm_get(s_pio, s_sm_rx);
        uint32_t word   = raw >> 23;
        uint8_t  byte   = (uint8_t)(word & 0xFF);
        uint8_t  id_bit = (uint8_t)((word >> 8) & 0x01);

        // Transmettre au parser BiDiB  (word contient data + id_bit)
            bidib_rx_buf_put((uint16_t)word);

        if (id_bit == 1) {
            if (byte & 0x40) {
                // Token système
                if (byte == BIDIBUS_LOGON) {
                    if (g_bidib_connect == BIDIB_DISCONNECTED) {
                        if (g_bidib_backoff) {
                            g_bidib_backoff--;
                        } else {
                            // Collision avoidance
                            busy_wait_us_32(1);
                            bool bus_free = true;
                            for (int i = 0; i < BIDIB_CA_TIME_US; i++) {
                                if (!gpio_get(BIDIB_PIN_RX)) {
                                    bus_free = false;
                                    break;
                                }
                                busy_wait_us_32(1);
                            }
                            if (bus_free) {
                                g_bidib_connect = BIDIB_APPLIED;
                                bidib_start_tx();  // ← non bloquant !
                            }
                        }
                    } else if (g_bidib_connect == BIDIB_APPLIED) {
                        // Laisser du temps au parser pour traiter le ACK
                        // avant de relancer un logon                      
                            g_bidib_backoff = (g_bidib_backoff + 1) & 0x3F;
                            g_bidib_connect = BIDIB_DISCONNECTED;                   
                    }
                }
            } else {
                // Adresse nœud
                if (byte == my_bidib_node_addr) {
                    // Poll → NODE_READY
                    gpio_put(BIDIB_PIN_DE, 1); 
                    pio_sm_put(s_pio, s_sm_tx, 0x000);
                    irq_set_enabled(PIO0_IRQ_1, true);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────
// Initialisation
// ─────────────────────────────────────────────────
void bidib_init(void)
{
    printf("bidib_init\n");
    

    bidib_prepare_logon_buf();

    gpio_init(BIDIB_PIN_DE);
    gpio_set_dir(BIDIB_PIN_DE, GPIO_OUT);
    gpio_put(BIDIB_PIN_DE, 0);
    printf("DE pin OK\n");

    uint offset_rx = pio_add_program(s_pio, &bidib_uart_rx_program);
    uint offset_tx = pio_add_program(s_pio, &bidib_uart_tx_program);

    bidib_uart_rx_init(s_pio, s_sm_rx, offset_rx, BIDIB_PIN_RX, BIDIB_BAUD);
    bidib_uart_tx_init(s_pio, s_sm_tx, offset_tx, BIDIB_PIN_TX, BIDIB_BAUD);

    printf("RX offset=%d pin=%d clkdiv=%.2f\n",
           offset_rx, BIDIB_PIN_RX,
           (float)clock_get_hz(clk_sys) / (BIDIB_BAUD * 8.0f));


    // Purger IRQ résiduelles
    pio_interrupt_clear(s_pio, 0);
    pio_interrupt_clear(s_pio, 1);

    // IRQ RX → PIO0_IRQ_0
    pio_set_irq0_source_enabled(s_pio, pis_interrupt0, true);
    irq_set_exclusive_handler(PIO0_IRQ_0, bidib_pio_rx_isr);
    irq_set_priority(PIO0_IRQ_0, 0);
    irq_set_enabled(PIO0_IRQ_0, true);

    // IRQ TX → PIO0_IRQ_1 (désactivée jusqu'au premier envoi)
    pio_set_irq1_source_enabled(s_pio, pis_interrupt1, true);
    irq_set_exclusive_handler(PIO0_IRQ_1, bidib_pio_tx_isr);
    irq_set_priority(PIO0_IRQ_1, 0);
    irq_set_enabled(PIO0_IRQ_1, false);  // activée par bidib_start_tx()

    printf("bidib_init done\n");

}