/*
 * bidib_client_parser.c  —  WiBiDiB (Pico 2W)
 *
 * Portage depuis OpenDCC/Atmel (Wolfgang Kufer) → Pico bare-metal
 * Copyright (c) 2010-2012 Wolfgang Kufer — GNU GPL v2
 * Portage + extension Distributed Control : Pierre Moulin 2025
 *
 * Rôle : parser les messages BiDiB reçus de l'IF2 et envoyer les
 *        commandes de traction (MSG_GUEST_REQ_SEND / MSG_CS_DRIVE)
 *
 * Simplifications vs Atmel :
 *   - cortos / t_cr_task   → void run_bidib_client() appelé dans main loop
 *   - flag_query/set/reset → variables bool
 *   - get_systick()        → now_ms() (time_us_64/1000)
 *   - led_*                → printf
 *   - _delay_ms()          → sleep_ms()
 *   - PROGMEM / pgmspace   → supprimé (Flash Pico accessible directement)
 *
 * Distributed Control (nouveau) :
 *   bidib_send_cs_drive()  → MSG_GUEST_REQ_SEND { TARGET_MODE_DCCGEN, MSG_CS_DRIVE }
 *   bidib_send_boost_state() → MSG_GUEST_REQ_SEND { TARGET_MODE_BOOSTER, MSG_BOOST_ON/OFF }
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "bidib_messages.h"
#include "bidib_client_if.h"
#include "bidib_client_parser.h"
#include "bidib.h"
#include "crc_8bit.h"
#include "config.h"
#include "crc_8bit.h"

// ─── Variables globales ───────────────────────────────────────────────────────

uint8_t g_bidib_connect  = BIDIB_DISCONNECTED;
uint8_t g_bidib_backoff  = 0;
uint8_t my_bidib_node_addr = 0xFF;

// Guest mode activé par MSG_SYS_ENABLE
static bool g_bidib_spontan_enabled = false;

// Numéro de séquence des messages RX attendus
static uint8_t bidib_rx_msg_num = 0;

// Numéro de séquence TX (partagé avec bidib_client_if.c via bidib_get_tx_num())
// déjà dans bidib_client_if.c

// UID unique du nœud (défini dans bidib.c / smartphone_if.c)
extern const uint8_t MyUniqueID[7];

// ─── Buffer de réception de paquets ──────────────────────────────────────────
// Un paquet BiDiB = PLENGTH + messages + CRC
static uint8_t  bidib_rx_paket[64];
static uint8_t  bidib_rx_index = 0;
static uint8_t  bidib_rx_total = 0;
static uint8_t  bidib_rx_crc   = 0;

// État machine RX
typedef enum {
    BIDIB_IDLE,
    BIDIB_GET_LEN,
    BIDIB_COLLECT_MESSAGE,
} bidib_rx_state_t;

static bidib_rx_state_t bidib_rx_state = BIDIB_IDLE;

// Timestamp pour watchdog connexion
static uint32_t bidib_active_timestamp = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline uint32_t get_tick_ms(void) {
    return (uint32_t)(time_us_64() / 1000ULL);
}

// ─── send_bidib_message() ────────────────────────────────────────────────────
// Point d'entrée unique pour envoyer un message BiDiB
// Identique Atmel — délègue à bidib_tx_fifo_put()

bool send_bidib_message(uint8_t *message) {
    return bidib_tx_fifo_put(message);
}

// ─── Messages système de base ────────────────────────────────────────────────

static void bidib_send_sys_magic(void) {
    // MSG_SYS_MAGIC répond avec 0xFE 0xAF (magic word BiDiB)
    t_node_message2 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_SYS_MAGIC;
    message.data[0] = 0xFE;
    message.data[1] = 0xAF;
    message.header.size = 3 + 2;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_sys_pong(uint8_t para) {
    t_node_message1 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_SYS_PONG;
    message.data = para;
    message.header.size = 3 + 1;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_sys_unique_id(void) {
    t_node_message10 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_SYS_UNIQUE_ID;
    memcpy(message.data, MyUniqueID, 7);
    message.header.size = 3 + 7;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_sys_pversion(void) {
    t_node_message2 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_SYS_P_VERSION;
    message.data[0] = 0;    // version majeure
    message.data[1] = 7;    // version mineure (BiDiB 0.7)
    message.header.size = 3 + 2;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_error(uint8_t error_num, uint8_t error_para) {
    t_node_message2 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_SYS_ERROR;
    message.data[0] = error_num;
    message.data[1] = error_para;
    message.header.size = 3 + 2;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_pkt_capacity(void) {
    t_node_message1 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = MSG_PKT_CAPACITY;
    message.data = 64;  // capacité paquet en octets
    message.header.size = 3 + 1;
    send_bidib_message((uint8_t *)&message);
}

bool bidib_send_onepara_msg(uint8_t msg_type, uint8_t dat) {
    t_node_message1 message;
    message.header.node_addr = 0;
    message.header.index     = bidib_get_tx_num();
    message.header.msg_type  = msg_type;
    message.data = dat;
    message.header.size = 3 + 1;
    return send_bidib_message((uint8_t *)&message);
}

// ─── set_bidib_state() ───────────────────────────────────────────────────────
// Gestion de l'état de connexion BiDiB
// Identique Atmel — sans LED, sans cortos

void set_bidib_state(uint8_t neu, uint8_t assigned_addr) {
    if (neu == g_bidib_connect) return;

    g_bidib_connect = neu;
    printf("[bidib_parser] state → %d\n", g_bidib_connect);

    switch (g_bidib_connect) {
        default:
        case BIDIB_DISCONNECTED:
            my_bidib_node_addr     = 0xFF;
            g_bidib_spontan_enabled = false;
            bidib_flush_rx();
            bidib_flush_tx();
            bidib_rx_state = BIDIB_IDLE;
            printf("[bidib_parser] DISCONNECTED\n");
            break;

        case BIDIB_APPLIED:
            printf("[bidib_parser] APPLIED — logon sent, waiting ACK\n");
            break;

        case BIDIB_CONNECTED:
            my_bidib_node_addr = assigned_addr;
            bidib_active_timestamp = get_tick_ms();
            printf("[bidib_parser] CONNECTED — node_addr=0x%02X\n",
                   my_bidib_node_addr);
            break;

        case BIDIB_REJECTED:
            my_bidib_node_addr     = 0xFF;
            g_bidib_spontan_enabled = false;
            printf("[bidib_parser] REJECTED\n");
            break;
    }
}

// ─── process_bidib_message() ─────────────────────────────────────────────────
// Parse un message BiDiB reçu de l'IF2
// Paramètre : pointeur vers le byte LENGTH du message
// Retour    : nombre d'octets consommés (length + 1)
// Identique Atmel — simplifié pour WiBiDiB (pas de CLASS_OCCUPANCY, etc.)

static uint8_t process_bidib_message(uint8_t *bidib_rx_msg) {
    uint8_t  length;
    uint8_t *msg_type;

    length = *bidib_rx_msg++;
    if ((length == 0) || (length & 0x80)) {
        bidib_send_error(BIDIB_ERR_SIZE, bidib_rx_msg_num);
        return 128;
    }

    // Vérification adresse
    if (*bidib_rx_msg == 0) {
        // broadcast
        bidib_rx_msg++;
    } else if (*bidib_rx_msg == (my_bidib_node_addr & 0x3F)) {
        bidib_rx_msg++;
        if (*bidib_rx_msg == 0) {
            // directement adressé à nous
            bidib_rx_msg++;
            bidib_rx_msg_num++;
            if (bidib_rx_msg_num == 0) bidib_rx_msg_num = 1;
            if (*bidib_rx_msg != bidib_rx_msg_num) {
                bidib_send_error(BIDIB_ERR_SEQUENCE, bidib_rx_msg_num);
                bidib_rx_msg_num = *bidib_rx_msg;
            }
        } else {
            return (length + 1);  // sub-node adressé — on n'en a pas
        }
    } else {
        return (length + 1);  // pas pour nous
    }

    // Pointer sur le type de message
    bidib_rx_msg++;
    msg_type = bidib_rx_msg;

    printf("[bidib_parser] rx msg type=0x%02X\n", *msg_type);

    switch (*msg_type) {

        // ── Système ──────────────────────────────────────────────────────────
        case MSG_SYS_GET_MAGIC:
            bidib_send_sys_magic();
            break;

        case MSG_SYS_GET_P_VERSION:
            bidib_send_sys_pversion();
            break;

        case MSG_SYS_PING:
            bidib_send_sys_pong(msg_type[1]);
            break;

        case MSG_SYS_GET_UNIQUE_ID:
            bidib_send_sys_unique_id();
            break;

        case MSG_SYS_ENABLE:
            // L'IF2 autorise les messages spontanés (guest mode)
            if (g_bidib_connect == BIDIB_CONNECTED) {
                g_bidib_spontan_enabled = true;
                printf("[bidib_parser] SYS_ENABLE → guest mode ON\n");
            }
            break;

        case MSG_SYS_DISABLE:
            g_bidib_spontan_enabled = false;
            printf("[bidib_parser] SYS_DISABLE → guest mode OFF\n");
            break;

        case MSG_SYS_RESET:
            printf("[bidib_parser] SYS_RESET → restarting\n");
            sleep_ms(500);
            // Reset Pico via watchdog
            // watchdog_reboot(0, 0, 0);  // décommenter si watchdog activé
            break;

        case MSG_GET_PKT_CAPACITY:
            bidib_send_pkt_capacity();
            break;

        case MSG_SYS_GET_ERROR:
            bidib_send_onepara_msg(MSG_SYS_ERROR, 0);
            break;

        // ── Logon ─────────────────────────────────────────────────────────────
        case MSG_LOGON_ACK:         // 0x70
        // send      0x0B 0A 00 00 F0 80 00 13 BA F1 86 BC 89
        // RX 0x100 0C 0B 00 00 70 01 80 00 13 BA F1 86 BC 46
            // msg_type[1] = NODE_ADDR assignée
            // msg_type[2..8] = UniqueID (7 octets)
            // On vérifie que l'UID correspond à la nôtre
            if (memcmp(msg_type + 2, MyUniqueID, 7) == 0) {
                uint8_t assigned = msg_type[1];
                // Calcul parité (identique Atmel)
                uint8_t temp = assigned ^ (assigned << 4);
                temp = temp ^ (temp << 2);
                temp = temp ^ (temp << 1);
                temp = temp & 0x80;
                set_bidib_state(BIDIB_CONNECTED, assigned | temp);
                printf("[bidib_parser] LOGON_ACK → addr=0x%02X\n", assigned);
            }
            break;

        case MSG_LOGON_REJECTED:
            if (memcmp(msg_type + 1, MyUniqueID, 7) == 0) {
                set_bidib_state(BIDIB_REJECTED, 0);
            }
            break;

        // ── Distributed Control — réponses de l'IF2 ──────────────────────────
        case MSG_GUEST_RESP_SENT:
            // Confirmation que l'IF2 a bien reçu notre REQ_SEND
            // msg_type[1] = RESULT (0=OK, autre=erreur)
            printf("[bidib_parser] GUEST_RESP_SENT result=0x%02X\n", msg_type[1]);
            break;

        case MSG_GUEST_RESP_NOTIFY:
            // Notification de l'IF2 sur un abonnement
            printf("[bidib_parser] GUEST_RESP_NOTIFY type=0x%02X\n", msg_type[1]);
            break;

        // ── Messages non gérés ────────────────────────────────────────────────
        default:
            printf("[bidib_parser] unhandled msg type=0x%02X\n", *msg_type);
            break;
    }

    return (length + 1);
}

// ─── bidib_parser() ──────────────────────────────────────────────────────────
// Parse un paquet complet (plusieurs messages possibles)

static void bidib_parser(void) {
    bidib_rx_total = bidib_rx_paket[0];
    bidib_rx_index = 1;  // sauter PLENGTH

    printf("[bidib_parser] parsing packet, total=%d\n", bidib_rx_total);

    while (bidib_rx_index < bidib_rx_total) {
        uint8_t consumed = process_bidib_message(
            &bidib_rx_paket[bidib_rx_index]);
        if (consumed == 128) break;  // erreur de longueur
        bidib_rx_index += consumed;
    }
}

// ─── run_bidib_client() ───────────────────────────────────────────────────────
// Boucle principale BiDiB — appelée depuis main() à chaque itération
// Équivalent de la t_cr_task Atmel — sans cortos

void run_bidib_client(void) {
    uint32_t now = get_tick_ms();

    // ── 1. Watchdog connexion ─────────────────────────────────────────────────
    if (g_bidib_connect == BIDIB_CONNECTED) {
        if ((now - bidib_active_timestamp) > 5000) {
            // Pas de message depuis 5s → déconnexion
            printf("[bidib_parser] timeout → DISCONNECTED\n");
            set_bidib_state(BIDIB_DISCONNECTED, 0);
        }
    }

    // ── 2. Lecture du buffer RX et assemblage des paquets ────────────────────
    while (bidib_rx_ready()) {
        uint16_t raw    = bidib_rx_read();
        uint8_t  byte   = (uint8_t)(raw & 0xFF);
        uint8_t  id_bit = (uint8_t)((raw >> 8) & 0x01);

        // Mise à jour timestamp activité
        bidib_active_timestamp = now;

        switch (bidib_rx_state) {
            case BIDIB_IDLE:
                // Seul id_bit=1 avec byte non nul démarre un paquet
                if (id_bit == 1 && byte == 0x00) {
                // 0x00 avec id=1 = début de paquet pour le nœud 0
                bidib_rx_state = BIDIB_GET_LEN;
                }
                break;
           
            case BIDIB_GET_LEN:
                if (id_bit == 0 && byte > 0) {
                    bidib_rx_paket[0] = byte;   // P_LENGTH paket length
                    bidib_rx_index    = 1;      // index dans le paquet (0 = P_LENGTH)
                    bidib_rx_crc      = crc_array[byte];
                    bidib_rx_state    = BIDIB_COLLECT_MESSAGE;
                    printf(" GL %02x crc %02x ", byte, bidib_rx_crc);
                } else {
                    bidib_rx_state = BIDIB_IDLE;
                }
                break;

            case BIDIB_COLLECT_MESSAGE:
                // bidib_rx_paket[0] = PLENGTH = nombre d'octets de données (sans CRC)
                // On attend PLENGTH octets de données (index 1..PLENGTH)
                // puis 1 octet CRC (index PLENGTH+1)
                // Total octets à recevoir après PLENGTH : PLENGTH + 1
                if (bidib_rx_index <= bidib_rx_paket[0]) {
                    // Octet de données
                    if (bidib_rx_index < 64) {
                        bidib_rx_paket[bidib_rx_index] = byte;
                    }
                    bidib_rx_crc = crc8_update(bidib_rx_crc, byte);
                    bidib_rx_index++;
// login P_LENGTH M_LENGTH 0x00 0x00 MSG_LOCAL_LOGON UNIQUE_ID CRC8
// P_LENGTH packet length, M_LENGTH message length
// In this case, UNIQUE_ID is 7 bytes  long, thus resulting in M_LENGTH of 10 and P_LENGTH of 11. 
// Address and MSG_NUM are here 0. The systematic structure of the logon message is identical to the normal messages.
// crc starts with package length, then all bytes of the message (including length and type) 
//                    
// (0x100) P_LENGTH 0C  M_LENGTH 0b  00  00  msg_log 70 adr 01 UID 80  00  13  ba  f1  b6  bc crc 46                    
                } else {
                    // bidib_rx_index == bidib_rx_paket[0] + 1 → octet CRC
                    // Le CRC est calculé sur tous les octets précédents
                    // (crc_array[PLENGTH] puis crc8_update pour chaque octet)
                    // En fin de paquet valide, crc8_update(crc_courant, CRC_reçu) == 0
                    uint8_t final_crc = crc8_update(bidib_rx_crc, byte);
                    if (final_crc == 0) {
                        bidib_parser();
                    } else {
                        printf("[bidib_parser] CRC error! expected=0x%02X got=0x%02X byte=0x%02X\n",
                               bidib_rx_crc, byte, final_crc);
                    }
                    bidib_rx_state = BIDIB_IDLE;
                }
                break;

            default:
                bidib_rx_state = BIDIB_IDLE;
                break;
        }
    }
}

// ─── init_bidib_client() ─────────────────────────────────────────────────────

void init_bidib_client(void) {
    g_bidib_connect         = BIDIB_DISCONNECTED;
    g_bidib_spontan_enabled = false;
    bidib_rx_state          = BIDIB_IDLE;
    bidib_rx_index          = 0;
    bidib_rx_msg_num        = 0;
    bidib_active_timestamp  = 0;
    init_bidib_client_if();
    printf("[bidib_parser] init done\n");
}

// ─── Distributed Control : bidib_send_cs_drive() ─────────────────────────────
//
// Traduit une commande WiThrottle (vitesse/direction/fonctions) en
// MSG_GUEST_REQ_SEND { TARGET_MODE_DCCGEN, MSG_CS_DRIVE, ... }
// envoyé à l'IF2 qui le transmet à la CS.
//
// Paramètres :
//   dcc_addr  : adresse DCC de la loco (14 bits)
//   speed     : 0..126 (128 steps) ou -1 emergency stop
//   dir       : 1=avant, 0=arrière
//   f         : tableau LocoState[29] (F0..F28)
//
// Format MSG_CS_DRIVE (d'après bidib_messages.h) :
//   [addrl][addrh][format][active][speed][f4_f0][f12_f5][f20_f13][f28_f21]
//
// Format MSG_GUEST_REQ_SEND :
//   [size][addr=0][mnum][MSG_GUEST_REQ_SEND]
//   [TARGET_MODE=0x0C]   ← BIDIB_TARGET_MODE_DCCGEN
//   [REQ_MSG_TYPE=MSG_CS_DRIVE]
//   [REQ_DATA...]         ← les 9 octets CS_DRIVE
//
void bidib_send_cs_drive(uint16_t dcc_addr, int speed, uint8_t dir,
                         int f[29]) {
    if (g_bidib_connect != BIDIB_CONNECTED) return;
    if (!g_bidib_spontan_enabled) {
        printf("[bidib_parser] cs_drive: guest mode not enabled yet\n");
        return;
    }
/*
    // Construire les données MSG_CS_DRIVE
    uint8_t addrl  = (uint8_t)(dcc_addr & 0xFF);
    uint8_t addrh  = (uint8_t)((dcc_addr >> 8) & 0x3F);

    // format : DCC 128 steps = 0x03
    uint8_t format = BIDIB_CS_DRIVE_FORMAT_DCC128;

    // active : on envoie toujours vitesse + fonctions
    uint8_t active = BIDIB_CS_DRIVE_EVENT_MOVE |
                     BIDIB_CS_DRIVE_EVENT_FUNC1 |
                     BIDIB_CS_DRIVE_EVENT_FUNC2 |
                     BIDIB_CS_DRIVE_EVENT_FUNC3 |
                     BIDIB_CS_DRIVE_EVENT_FUNC4;

    // speed : bit7 = direction (1=avant), bits 0-6 = vitesse
    uint8_t speed_byte;
    if (speed < 0) {
        speed_byte = 0x01;  // emergency stop
    } else {
        speed_byte = (uint8_t)(speed & 0x7F);
        if (dir) speed_byte |= 0x80;
    }

    // fonctions
    uint8_t f4_f0  = (f[0] ? 0x10 : 0)
                   | (f[1] ? 0x01 : 0)
                   | (f[2] ? 0x02 : 0)
                   | (f[3] ? 0x04 : 0)
                   | (f[4] ? 0x08 : 0);

    uint8_t f12_f5 = (f[5]  ? 0x01 : 0)
                   | (f[6]  ? 0x02 : 0)
                   | (f[7]  ? 0x04 : 0)
                   | (f[8]  ? 0x08 : 0)
                   | (f[9]  ? 0x10 : 0)
                   | (f[10] ? 0x20 : 0)
                   | (f[11] ? 0x40 : 0)
                   | (f[12] ? 0x80 : 0);

    uint8_t f20_f13= (f[13] ? 0x01 : 0)
                   | (f[14] ? 0x02 : 0)
                   | (f[15] ? 0x04 : 0)
                   | (f[16] ? 0x08 : 0)
                   | (f[17] ? 0x10 : 0)
                   | (f[18] ? 0x20 : 0)
                   | (f[19] ? 0x40 : 0)
                   | (f[20] ? 0x80 : 0);

    uint8_t f28_f21= (f[21] ? 0x01 : 0)
                   | (f[22] ? 0x02 : 0)
                   | (f[23] ? 0x04 : 0)
                   | (f[24] ? 0x08 : 0)
                   | (f[25] ? 0x10 : 0)
                   | (f[26] ? 0x20 : 0)
                   | (f[27] ? 0x40 : 0)
                   | (f[28] ? 0x80 : 0);

    // Construire MSG_GUEST_REQ_SEND
    // [size][addr=0][mnum][MSG_GUEST_REQ_SEND]
    // [TARGET_MODE_DCCGEN=0x0C]
    // [MSG_CS_DRIVE]
    // [addrl][addrh][format][active][speed][f4_f0][f12_f5][f20_f13][f28_f21]
    //
    // size = 3 (header sans size) + 1 (TARGET_MODE) + 1 (REQ_MSG_TYPE) + 9 (CS_DRIVE data)
    //      = 14

    uint8_t msg[16];
    uint8_t idx = 0;

    msg[idx++] = 3 + 1 + 1 + 9;          // size = 14
    msg[idx++] = 0;                        // addr = 0 (vers host)
    msg[idx++] = bidib_get_tx_num();       // mnum
    msg[idx++] = MSG_GUEST_REQ_SEND;       // message type
    msg[idx++] = BIDIB_TARGET_MODE_DCCGEN; // 0x0C
    msg[idx++] = MSG_CS_DRIVE;             // commande encapsulée
    msg[idx++] = addrl;
    msg[idx++] = addrh;
    msg[idx++] = format;
    msg[idx++] = active;
    msg[idx++] = speed_byte;
    msg[idx++] = f4_f0;
    msg[idx++] = f12_f5;
    msg[idx++] = f20_f13;
    msg[idx++] = f28_f21;

    printf("[bidib_parser] cs_drive addr=%d spd=%d dir=%d f4f0=0x%02X\n",
           dcc_addr, speed, dir, f4_f0);
*/
    
t_bidib_cs_drive drive;
drive.addr    = dcc_addr;
drive.format  = BIDIB_CS_DRIVE_FORMAT_DCC128;
drive.active  = BIDIB_CS_DRIVE_SPEED_BIT
              | BIDIB_CS_DRIVE_F1F4_BIT
              | BIDIB_CS_DRIVE_F5F8_BIT
              | BIDIB_CS_DRIVE_F9F12_BIT
              | BIDIB_CS_DRIVE_F13F20_BIT
              | BIDIB_CS_DRIVE_F21F28_BIT;
drive.speed   = (speed < 0) ? 0x01 : ((uint8_t)(speed & 0x7F) | (dir ? 0x80 : 0));
drive.f4_f0   = (f[0] ? 0x10 : 0) | (f[1]?0x01:0) | (f[2]?0x02:0)
              | (f[3]?0x04:0) | (f[4]?0x08:0);
drive.f12_f5  = (f[5]?0x01:0) | (f[6]?0x02:0) | (f[7]?0x04:0) | (f[8]?0x08:0)
              | (f[9]?0x10:0) | (f[10]?0x20:0) | (f[11]?0x40:0) | (f[12]?0x80:0);
drive.f20_f13 = (f[13]?0x01:0) | (f[14]?0x02:0) | (f[15]?0x04:0) | (f[16]?0x08:0)
              | (f[17]?0x10:0) | (f[18]?0x20:0) | (f[19]?0x40:0) | (f[20]?0x80:0);
drive.f28_f21 = (f[21]?0x01:0) | (f[22]?0x02:0) | (f[23]?0x04:0) | (f[24]?0x08:0)
              | (f[25]?0x10:0) | (f[26]?0x20:0) | (f[27]?0x40:0) | (f[28]?0x80:0);

// Construire MSG_GUEST_REQ_SEND
uint8_t msg[16];
uint8_t idx = 0;
msg[idx++] = 3 + 1 + 1 + sizeof(t_bidib_cs_drive);  // size
msg[idx++] = 0;                                        // addr
msg[idx++] = bidib_get_tx_num();                       // mnum
msg[idx++] = MSG_GUEST_REQ_SEND;
msg[idx++] = BIDIB_TARGET_MODE_DCCGEN;                 // 0x0C
msg[idx++] = MSG_CS_DRIVE;
memcpy(&msg[idx], &drive, sizeof(t_bidib_cs_drive));

send_bidib_message(msg);
}

// ─── bidib_send_boost_state() ────────────────────────────────────────────────
// Traduit PPA0/PPA1 (Engine Driver) en MSG_GUEST_REQ_SEND { BOOSTER, BOOST_ON/OFF }

void bidib_send_boost_state(uint8_t on) {
    if (g_bidib_connect != BIDIB_CONNECTED) return;

    uint8_t msg[8];
    uint8_t idx = 0;

    msg[idx++] = 3 + 1 + 1;              // size = 5
    msg[idx++] = 0;                        // addr
    msg[idx++] = bidib_get_tx_num();
    msg[idx++] = MSG_GUEST_REQ_SEND;
    msg[idx++] = BIDIB_TARGET_MODE_BOOSTER; // 0x09
    msg[idx++] = on ? MSG_BOOST_ON : MSG_BOOST_OFF;

    printf("[bidib_parser] boost_state %s\n", on ? "ON" : "OFF");
    send_bidib_message(msg);
}
