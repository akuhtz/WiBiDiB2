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
#include "hardware/timer.h" 

static const char *TAG = "bidib_client_parser";

// ─── Variables globales ───────────────────────────────────────────────────────

uint8_t g_bidib_connect  = BIDIB_DISCONNECTED;
uint8_t g_bidib_backoff  = 0;
uint8_t my_bidib_node_addr = 0xFF;
uint8_t my_addr_stack[4] = {0, 0, 0, 0};
uint8_t my_addr_depth    = 0;

volatile uint8_t g_bidib_guest_enabled = 0;
static bool guest_subscribed = false;

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


// ─── Helpers ─────────────────────────────────────────────────────────────────



// Helper générique — construit l'en-tête dans buf, retourne l'offset des data
static uint8_t bidib_build_header(uint8_t *buf, uint8_t msg_type, uint8_t nb_data) {
  uint8_t i = 0;
    buf[i++] = 1 + 1 + 1 + nb_data;  // addr + index + type + data
    buf[i++] = 0x00;              // addr vers le maître, toujours 0
    buf[i++] = bidib_get_tx_num(); // index
    buf[i++] = msg_type;           // type
    return i;                      // offset pour les data
}
// ─── send_bidib_message() ────────────────────────────────────────────────────
// Point d'entrée unique pour envoyer un message BiDiB
// Identique Atmel — délègue à bidib_tx_fifo_put()

bool send_bidib_message(uint8_t *message) {
    
     gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(2);
    gpio_put(BIDIB_PIN_TEST , 0);
    
// LOG_INFO(TAG, " in send_bidib_message mg%s ", message);

    return bidib_tx_fifo_put(message);
    
    return bidib_tx_fifo_put(message);
}

// ─── Messages système de base ────────────────────────────────────────────────

    // MSG_SYS_MAGIC répond avec 0xFE 0xAF (magic word BiDiB)
static void bidib_send_sys_magic(void) {  
    uint8_t message[10];
    uint8_t i = bidib_build_header(message, MSG_SYS_MAGIC, 2);
    message[i++] = 0xFE;
    message[i++] = 0xAF;
  //  message.header.size = my_addr_depth + 1 + 1 + 1 + 2; // addr + term + index + type + data
 #if (DEBUG == 1)
  uint8_t *p = (uint8_t *)&message;
    printf("magic raw: %02X %02X %02X %02X %02X %02X %02X %02X\n",
        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
 # endif
    send_bidib_message((uint8_t *)&message);
}    

static void bidib_send_sys_pong(uint8_t para) {
    uint8_t message[10];
    uint8_t i = bidib_build_header(message, MSG_SYS_PONG, 3);
    message[i++] = para;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_sys_unique_id(void) {
    uint8_t message[10];
    uint8_t i = bidib_build_header(message, MSG_SYS_UNIQUE_ID, 10);
    message[i++] = MyUniqueID [0]; 
    message[i++] = MyUniqueID [1];
    message[i++] = MyUniqueID [2];
    message[i++] = MyUniqueID [3]; 
    message[i++] = MyUniqueID [4];
    message[i++] = MyUniqueID [5];
    message[i++] = MyUniqueID [6];  
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_sw_version(void) {
    uint8_t message[8];
    uint8_t i = bidib_build_header(message, MSG_SYS_SW_VERSION, 3);
    message[i++] = 0x00;  // patch
    message[i++] = 0x01;  // minor
    message[i++] = 0x00;  // major
    send_bidib_message(message);
}

static void bidib_send_sys_pversion(void) {
    uint8_t message[8];
    uint8_t i = bidib_build_header(message, MSG_SYS_P_VERSION, 2);
    message[i++] = 0x07;  // version mineure BiDiB 0.7
    message[i++] = 0x00;  // version majeure
    send_bidib_message(message);
}

static void bidib_send_feature_count(uint8_t count) {
    uint8_t message[8];
    uint8_t i = bidib_build_header(message, MSG_FEATURE_COUNT, 1);
    message[i++] = count;
    send_bidib_message(message);
}

/*  not needed pico has no node attached to it
static void bidib_send_nodetab_count(void) {
    uint8_t message[8];
    uint8_t i = bidib_build_header(message, MSG_NODETAB_COUNT, 1);
    message[i++] = 1;  // 1 nœud — le Pico lui-même
    send_bidib_message(message);
}

static void bidib_send_nodetab(void) {
    uint8_t message[16];
    uint8_t i = bidib_build_header(message, MSG_NODETAB, 9);
    message[i++] = 1;           // version table
    for (uint8_t  j = 0; j < 7; j++)
        message[i++] = MyUniqueID[j];  // UID[7]
    send_bidib_message(message);
}
*/

static void bidib_send_error(uint8_t error_num, uint8_t error_para) {
    uint8_t message[8];
    uint8_t i = bidib_build_header(message, MSG_SYS_ERROR, 2);
     message[i++] = error_num;
     message[i++] = error_para;
    send_bidib_message((uint8_t *)&message);
}

static void bidib_send_pkt_capacity(void) {
   uint8_t message[5];
    uint8_t i = bidib_build_header(message, MSG_PKT_CAPACITY, 1);
    message[i++] = 64;  // capacité paquet en octets
    send_bidib_message((uint8_t *)&message);
}

bool bidib_send_onepara_msg(uint8_t msg_type, uint8_t dat) {
    uint8_t message[5];
    uint8_t i = bidib_build_header(message, msg_type, 1);
    message[i++]= dat;
    return send_bidib_message((uint8_t *)&message);
}

// ─── set_bidib_state() ───────────────────────────────────────────────────────
// Gestion de l'état de connexion BiDiB
// Identique Atmel — sans LED, sans cortos

void set_bidib_state(uint8_t neu, uint8_t assigned_addr) {
    if (neu == g_bidib_connect) return;

    g_bidib_connect = neu;
    #if (DEBUG == 1)
    LOG_INFO(TAG,"state → %d\n",g_bidib_connect);
    #endif

    switch (g_bidib_connect) {
        default:
        case BIDIB_DISCONNECTED:
            my_bidib_node_addr     = 0xFF;
            g_bidib_spontan_enabled = false;
            bidib_flush_rx();
            bidib_flush_tx();
            bidib_rx_state = BIDIB_IDLE;
            #if (DEBUG == 1)
            LOG_INFO(TAG," DISCONNECTED");
            #endif
            break;

        case BIDIB_APPLIED:
            #if (DEBUG == 1)
                LOG_INFO(TAG,"APPLIED — logon sent, waiting ACK");
            #endif
            break;

        case BIDIB_CONNECTED:
            my_bidib_node_addr = assigned_addr;
            bidib_rx_msg_num = 0;
            bidib_tx0_msg_num = 0;
            last_poll_us = time_us_64();  // ← initialiser ici
            break;

        case BIDIB_REJECTED:
            my_bidib_node_addr     = 0xFF;
            g_bidib_spontan_enabled = false;
            #if (DEBUG == 1) 
                LOG_INFO(TAG," REJECTED");
            #endif
            break;
    }
}

#if (BIDIB_DISTRIBUTED_CONTROL == 1)
static bool bidib_guest_req_send(uint8_t msg_type_target,
                                  uint8_t target_mode,
                                  uint8_t *msg_data,
                                  uint8_t data_len) {
    uint8_t message[32];
    uint8_t i = bidib_build_header(message, MSG_GUEST_REQ_SEND, 3 + data_len);
    message[i++] = target_mode;      // TARGET_MODE
    message[i++] = 0x00;             // addr end (pas de nœud cible spécifique)
    message[i++] = msg_type_target;  // type du message cible (ex: MSG_CS_DRIVE)
    for (uint8_t j = 0; j < data_len; j++)
        message[i++] = msg_data[j];
    return send_bidib_message(message);
}

static void bidib_guest_req_subscribe(uint8_t class, uint8_t sub, uint8_t obj_count) {
    uint8_t message[12];
    uint8_t i = bidib_build_header(message, MSG_GUEST_REQ_SUBSCRIBE, 5);
    message[i++] = 0x00;        // TARGET_MODE_ABSOLUTE
    message[i++] = 0x00;        // addr end (pas de nœud cible spécifique)
    message[i++] = class;       // classe à abonner
    message[i++] = sub;         // direction upstream/downstream
    message[i++] = obj_count;   // nombre d'objets (0x0F = illimité)
    send_bidib_message(message);
}

#endif
// ─── process_bidib_message() ─────────────────────────────────────────────────
// Parse un message BiDiB reçu de l'IF2
// Paramètre : pointeur vers le byte LENGTH du message
// Retour    : nombre d'octets consommés (length + 1)
// Identique Atmel — simplifié pour WiBiDiB (pas de CLASS_OCCUPANCY, etc.)

static uint8_t process_bidib_message(uint8_t *bidib_rx_msg) {
   uint8_t  length;
    uint8_t *msg_type;
    uint8_t  addr_stack[4] = {0, 0, 0, 0};  // pile d'adresses extraite
    uint8_t  addr_depth = 0;                 // nombre de niveaux

    length = *bidib_rx_msg++;
    if ((length == 0) || (length & 0x80)) {
        #if (DEBUG == 1)
            LOG_INFO(TAG,"invalid message length: %d",length);
        #endif  
        return 128;
    }

    // Vérification adresse et extraction de la pile
    if (*bidib_rx_msg == 0) {
        // broadcast
        bidib_rx_msg++;  // saute addr=0x00
        bidib_rx_msg++;  // saute msg_num
    } else if (*bidib_rx_msg == (my_bidib_node_addr & 0x3F)) {
        // Extraire la pile d'adresses jusqu'au terminateur 0x00
        while (*bidib_rx_msg != 0x00 && addr_depth < 4) {
            addr_stack[addr_depth++] = *bidib_rx_msg++;
        }
        bidib_rx_msg++;  // saute le terminateur 0x00

        // Vérification séquence
        if (*bidib_rx_msg != bidib_rx_msg_num) {
#if (DEBUG == 1)
        LOG_INFO(TAG,"sequence resync: expected %d got %d",
             bidib_rx_msg_num, *bidib_rx_msg);
#endif
            bidib_rx_msg_num = *bidib_rx_msg;
        }
        bidib_rx_msg_num++;
        if (bidib_rx_msg_num == 0) bidib_rx_msg_num = 1;
        bidib_rx_msg++;  // saute msg_num
    } else {
        return (length + 1);  // pas pour nous
    }

    // Pointer sur le type de message
    msg_type = bidib_rx_msg;

    #if (DEBUG == 1)
    LOG_INFO(TAG,"rx msg type=0x%02X addr=[%d]",*msg_type, addr_stack[0]);
    #endif

    switch (*msg_type) {

        // ── Système ──────────────────────────────────────────────────────────
        case MSG_SYS_GET_MAGIC:
            /*
            gpio_put(BIDIB_PIN_TEST , 1);
            busy_wait_us_32(4);
            gpio_put(BIDIB_PIN_TEST , 0);
            */
        #if (DEBUG == 1)       
        printf("addr_stack=%02X depth=%d\n", my_addr_stack[0], my_addr_depth);
        #endif
          bidib_send_sys_magic();

            break;

        case MSG_SYS_GET_SW_VERSION:   // 0x06
            bidib_send_sw_version();
            break;
                
        case MSG_SYS_GET_P_VERSION:
            bidib_send_sys_pversion();
            break;

        case MSG_FEATURE_GETALL:   // 0x10
            bidib_send_feature_count(0);  // 0 features pour l'instant
            break;
/*  not needed pico has no node attached
        case MSG_NODETAB_GETALL:   // 0x0B
            bidib_send_nodetab_count();
            break;

        case MSG_NODETAB_GETNEXT:  // 0x0C
            bidib_send_nodetab();
            break;
*/
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
                #if (BIDIB_DISTRIBUTED_CONTROL == 1)
            if (!guest_subscribed) {
            bidib_guest_req_subscribe(CLASS_REQ_DCC_SIGNAL,
                                      SUBSCRIPTION_REQ_DOWNSTREAM, 0x0F);
            guest_subscribed = true;
        }
        #endif
            }
            break;

        case MSG_SYS_DISABLE:
            g_bidib_spontan_enabled = false;
            printf("[bidib_parser] SYS_DISABLE → guest mode OFF\n");
            break;

        case MSG_SYS_RESET:
            printf("[bidib_parser] SYS_RESET → restarting\n");
            sleep_ms(500);
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
                // Initialiser la pile d'adresses permanente
                memset(my_addr_stack, 0, 4);
                my_addr_stack[0] = assigned & 0x3F;  // adresse sans bit de parité
                my_addr_depth    = 1;

                printf("[bidib_parser] LOGON_ACK → addr=0x%02X\n", assigned);
                bidib_rx_msg_num = 0;  // repart de zéro à chaque connexion
                bidib_tx0_msg_num  = 0;
                g_bidib_guest_enabled = 0;
                guest_subscribed = false;
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

        case MSG_GUEST_RESP_SUBSCRIPTION:   // 0xD1
            {
                uint8_t *p = msg_type + 1;
                uint8_t targetMode = *p++;
                while (*p != 0x00) p++;   // saute la pile d'adresses jusqu'au terminateur
                p++;                       // saute le terminateur lui-même
                uint8_t ackApproval = *p++;
                uint8_t cls         = *p++;
                printf("[bidib_parser] GUEST_RESP_SUBSCRIPTION target=0x%02X ack=0x%02X class=0x%02X\n",
                    targetMode, ackApproval, cls);
                if (ackApproval == SUBSCRIPTION_ACK_OK || ackApproval == SUBSCRIPTION_ACK_CHANGED) {
                    g_bidib_guest_enabled = 1;
                    printf("[bidib_parser] guest subscription confirmed → CS_DRIVE enabled\n");
                } else {
                    printf("[bidib_parser] guest subscription REFUSED (ack=0x%02X)\n", ackApproval);
                }
                break;
            }

        // ── Messages non gérés ────────────────────────────────────────────────
        case MSG_SYS_CLOCK:   // 0x18
            break;
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
    gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(8);
    gpio_put(BIDIB_PIN_TEST , 0);
 #if (DEBUG == 1)
    printf("[bidib_parser] parsing packet, total=%d\n", bidib_rx_total);
#endif
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
  
  // -- 1. Check connected
  // utilisez now_ms() pour le test de timeout, et pas time_us_64() directement pour éviter overflow
    if (g_bidib_connect == BIDIB_CONNECTED) {
        uint32_t now = now_ms();
        uint32_t last_ms = (uint32_t)(last_poll_us / 1000ULL);

        if (now > last_ms && (now - last_ms) > 500) {
            printf("[timeout] now=%lu last=%lu diff=%lu\n", now, last_ms, now - last_ms);
            set_bidib_state(BIDIB_DISCONNECTED, 0);
            last_poll_us = time_us_64();
        }
    }

   
    // ── 2. Lecture du buffer RX et assemblage des paquets ────────────────────
    while (bidib_rx_ready()) {
        uint16_t raw    = bidib_rx_read();
        uint8_t  byte   = (uint8_t)(raw & 0xFF);
        uint8_t  id_bit = (uint8_t)((raw >> 8) & 0x01);

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
                    gpio_put(BIDIB_PIN_TEST , 1);
                    busy_wait_us_32(2);
                    gpio_put(BIDIB_PIN_TEST , 0);
                    #if (DEBUG == 1)
                        printf(" GL %02x crc %02x ", byte, bidib_rx_crc);
                    #endif
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
                        #if (DEBUG == 1)
                            printf("%02x ", byte);
                        #endif
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
void bidib_send_cs_drive(uint16_t dcc_addr, int speed, uint8_t dir, int f[29], uint8_t active) {
 // LOG_INFO(TAG,"in bidib_send_cs_drive dcc_addr %02x, speed %02x, dir %02x, active %02x",
 //                dcc_addr, speed,dir,  active);  
gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(6);
gpio_put(BIDIB_PIN_TEST , 0);

   //LOG_INFO(TAG,"g_bidib_connect %02x g_bidib_spontan_enabled %02x ",
   //          g_bidib_connect,g_bidib_spontan_enabled);
    if (g_bidib_connect != BIDIB_CONNECTED) return;
    if (!g_bidib_spontan_enabled) return;

    t_bidib_cs_drive drive;
    drive.addr    = dcc_addr;
    drive.format  = BIDIB_CS_DRIVE_FORMAT_DCC128;
    drive.active  = active;
    drive.speed   = (speed < 0) ? 0x01 : ((uint8_t)(speed & 0x7F) | (dir ? 0x80 : 0));
    drive.f4_f0   = (f[0]?0x10:0)|(f[1]?0x01:0)|(f[2]?0x02:0)|(f[3]?0x04:0)|(f[4]?0x08:0);
    drive.f12_f5  = (f[5]?0x01:0)|(f[6]?0x02:0)|(f[7]?0x04:0)|(f[8]?0x08:0)
                   |(f[9]?0x10:0)|(f[10]?0x20:0)|(f[11]?0x40:0)|(f[12]?0x80:0);
    drive.f20_f13 = (f[13]?0x01:0)|(f[14]?0x02:0)|(f[15]?0x04:0)|(f[16]?0x08:0)
                   |(f[17]?0x10:0)|(f[18]?0x20:0)|(f[19]?0x40:0)|(f[20]?0x80:0);
    drive.f28_f21 = (f[21]?0x01:0)|(f[22]?0x02:0)|(f[23]?0x04:0)|(f[24]?0x08:0)
                   |(f[25]?0x10:0)|(f[26]?0x20:0)|(f[27]?0x40:0)|(f[28]?0x80:0);
                 

#if (BIDIB_DISTRIBUTED_CONTROL == 1)
    if (!g_bidib_guest_enabled) {
        printf("[cs_drive] guest not subscribed yet — drop\n");
        return;
    }
    bidib_guest_req_send(MSG_CS_DRIVE,
                         BIDIB_TARGET_MODE_DISPATCH_DCCGEN,
                         (uint8_t *)&drive,
                         sizeof(t_bidib_cs_drive));
#else

    // Envoi direct MSG_CS_DRIVE sans distributed control
    uint8_t message[16];
    uint8_t i = bidib_build_header(message, MSG_CS_DRIVE, sizeof(t_bidib_cs_drive));
    memcpy(&message[i], &drive, sizeof(t_bidib_cs_drive));
 //   LOG_INFO(TAG,"send_bidib_message %s", message);
 gpio_put(BIDIB_PIN_TEST , 1);
    busy_wait_us_32(2);
gpio_put(BIDIB_PIN_TEST , 0);     
    send_bidib_message(message);
#endif
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
