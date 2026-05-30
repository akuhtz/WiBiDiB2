/*
 * withrottle_if.c  —  WiBiDiB  (Pico 2W)
 *
 * Passerelle Engine Driver (WiThrottle) → BiDiBus
 * Copyright (c) 2010 Pierre Moulin — GNU GPL v2
 *
 * Nettoyage v2 :
 *   - Suppression code WiThrottle (MT+/MTL) — Engine Driver uniquement (M0+/M0L)
 *   - Suppression isWT, MAX_LOCO_PER_THR=1 par slot (plus de doublon WT/ED)
 *   - Correction bug actionKey '*' dans locoAction()
 *   - Correction réponse qV/qR (utilisait ak au lieu de Loco_actionKey)
 *   - Suppression ancien locoAdd commenté
 *   - TODO PHASE 2 : remplacer printf <t> et <f> par appels BiDiB
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>

#include "pico/stdlib.h"
#include "config.h"
#include "datatypes.h"
#include "withrottle_if.h"
#include "smartphone_if.h"
#include "tcp_server.h"
#include "bidib_client_parser.h"


static const char *TAG = "withrottle_if";

// ─── Structure loco ───────────────────────────────────────────────────────────
// 1 loco par slot (Engine Driver uniquement)
typedef struct {
    char    Loco_actionKey[16]; // ex: "S14" ou "L4550"
    int     LocoState[29];      // état F0..F28
    int     dccAdress;
    int     newSpeed;
    int     oldSpeed;
    uint8_t dir;                // 1=avant, 0=arrière
    uint8_t slot;
} Engine_t;

static Engine_t Loco[MAX_THROTTLES];

static const int heartbeatTimeout = HEARTBEAT_TIMEOUT_S;
static uint8_t   heartbeatEnable[MAX_THROTTLES];
static uint32_t  heartbeat[MAX_THROTTLES];

// Variables de parsing
static char    LocoAdress_g[14];
static char    actionKey_g[14];
static int     Throttle_g = 0;

// ─── throttle_stop() ─────────────────────────────────────────────────────────
void throttle_stop(uint8_t slot) {
    printf("[withrottle_if] throttle_stop slot %d\n", slot);
    heartbeatEnable[slot] = false;
    heartbeat[slot]       = 0;
    Loco[slot].newSpeed   = 0;
}

// ─── locoAdd() ───────────────────────────────────────────────────────────────
// Engine Driver uniquement : M0+ / M0L / V0 / R1 / s1
static void locoAdd(const char *th, const char *ak, uint8_t slot) {
    char    msg[128];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;

    memset(msg, 0, sizeof(msg));
    memcpy(Loco[slot].Loco_actionKey, ak, strlen(ak) + 1);

    // M0+<ak><;>\n\n
    msg[0]='M'; msg[1]='0'; msg[2]='+'; length_msg=3;
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // M0L<ak><;>]\[Feux]\[]\[]\[]...\[\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; msg[1]='0'; msg[2]='L'; length_msg=3;
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]=']'; msg[length_msg++]='\\'; msg[length_msg++]='[';
    msg[length_msg++]='F'; msg[length_msg++]='e'; msg[length_msg++]='u';
    msg[length_msg++]='x'; msg[length_msg++]=']';
    for (uint8_t c = 0; c < 27; c++) {
        msg[length_msg++]='\\'; msg[length_msg++]='['; msg[length_msg++]=']';
    }
    msg[length_msg++]='\\'; msg[length_msg++]='[';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // RAZ état locos en mémoire
    for (int fk = 0; fk < 29; fk++) Loco[slot].LocoState[fk] = 0;
    Loco[slot].newSpeed = 0;
    Loco[slot].oldSpeed = 0;
    Loco[slot].dir      = 1;

    // M0A<ak><;>V0\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='V'; msg[length_msg++]='0';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // M0A<ak><;>R1\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='R'; msg[length_msg++]='1';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // M0A<ak><;>s1\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='s'; msg[length_msg++]='1';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    tcp_output(pcb);
    printf("[withrottle_if] locoAdd %s dcc=%d\n", ak, Loco[slot].dccAdress);
}

// ─── locoRelease() ───────────────────────────────────────────────────────────
static void locoRelease(const char *th, const char *ak, uint8_t slot) {
    char    msg[64];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;

    heartbeat[slot]     = 0;
    Loco[slot].newSpeed = 0;

    // M0-<ak><;>\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='-';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);
    tcp_output(pcb);
}

// ─── locoAction() ────────────────────────────────────────────────────────────
static void locoAction(const char *th, char *ak, uint8_t slot) {
    char    msg[128];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;
    char    tmp[20]     = {};
    char    locoAddress[10] = {};

    printf("[withrottle_if] locoAction th=%s ak=%s\n", th, ak);
    snprintf(locoAddress, sizeof(locoAddress), "%d", Loco[slot].dccAdress);

    // '*' : remplacer par l'actionKey réelle sauvegardée
    if (ak[0] == '*') {
        memcpy(ak, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey) + 1);
    }

    // ── F : fonction ──────────────────────────────────────────────────────────
    if (ak[0] == 'F') {
        // Format : F<0|1><num>  ex: F10 = F0 on, F110 = F10 on
        int fstate = ak[1] - '0';               // 0 ou 1
        int fk     = (int)strtol(ak + 2, NULL, 10);
        Loco[slot].LocoState[fk] = fstate;

        // Renvoyer l'état confirmé au smartphone : M0A<ak><;>F<state><num>
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        msg[length_msg++]='F';
        itoa(fstate, tmp, 10);
        memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        itoa(fk, tmp, 10);
        memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        msg[length_msg++]='\n'; msg[length_msg++]='\n';
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);

        bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed,
                        Loco[slot].dir, Loco[slot].LocoState);
        
    }

    // ── qV / qR : demande de l'état courant ──────────────────────────────────
    else if (ak[0] == 'q') {
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        // Répondre avec l'actionKey réelle (ex: S14), pas 'q'
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        if (ak[1] == 'V') {
            msg[length_msg++]='V';
            itoa(Loco[slot].newSpeed, tmp, 10);
            memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        } else { // qR
            msg[length_msg++]='R';
            itoa(Loco[slot].dir, tmp, 10);
            memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        }
        msg[length_msg++]='\n'; msg[length_msg++]='\n';
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }

    // ── V : vitesse ───────────────────────────────────────────────────────────
    else if (ak[0] == 'V') {
        int spd = (int)strtol(ak + 1, NULL, 10);
        Loco[slot].newSpeed = spd;
        bidib_send_cs_drive(Loco[slot].dccAdress, spd, Loco[slot].dir, Loco[slot].LocoState);
        // Renvoyer V confirmée au smartphone
    }

    // ── R : direction ─────────────────────────────────────────────────────────
    else if (ak[0] == 'R') {
        int dir = (int)strtol(ak + 1, NULL, 10);
        Loco[slot].dir      = dir;
        Loco[slot].newSpeed = 0;
        bidib_send_cs_drive(Loco[slot].dccAdress, 0, dir, Loco[slot].LocoState);

        // Renvoyer V0 au smartphone
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        msg[length_msg++]='V'; msg[length_msg++]='0';
        msg[length_msg++]='\n'; msg[length_msg++]='\n';
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }

    // ── X : emergency stop ────────────────────────────────────────────────────
    else if (ak[0] == 'X') {
        Loco[slot].newSpeed = 0;
       bidib_send_cs_drive(Loco[slot].dccAdress, -1, Loco[slot].dir, Loco[slot].LocoState);
    }

    // ── I : idle ──────────────────────────────────────────────────────────────
    else if (ak[0] == 'I') {
        Loco[slot].newSpeed = 0;
        bidib_send_cs_drive(Loco[slot].dccAdress, 0, Loco[slot].dir, Loco[slot].LocoState);
    }

    // ── Q : quit loco ─────────────────────────────────────────────────────────
    else if (ak[0] == 'Q') {
        Loco[slot].newSpeed = 0;
        printf("dcc %d QUIT\n", Loco[slot].dccAdress);
    }
}

// ─── checkHeartbeat() ────────────────────────────────────────────────────────
static void checkHeartbeat(uint8_t slot) {
    char    msg[50];
    uint8_t length_msg;
    struct tcp_pcb *pcb = throttle[slot].pcb;
    char    tmp[20]     = {};
    uint32_t now        = now_ms();

    if (heartbeat[slot] > 0 &&
        (heartbeat[slot] + (uint32_t)heartbeatTimeout * 5000) < now) {

        Loco[slot].newSpeed = 0;
        heartbeat[slot]     = 0;

        // Envoyer V0 emergency
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; msg[1]='0'; msg[2]='A'; length_msg=3;
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        const char suffix[] = {'<',';','>','V','0','\n','\n'};
        memcpy(msg+length_msg, suffix, sizeof(suffix));
        length_msg += sizeof(suffix);
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }
}

// ─── process_rx_withrottle() ─────────────────────────────────────────────────
void process_rx_withrottle(rx_data_t *data, uint8_t slot) {
    char actionData[30] = {};
    int  delimiter      = 0;

    LOG_INFO(TAG, "<- slot %d message: %s", slot, data->msg);

    throttle[slot].state = NODE_LOGGED_ON;
    struct tcp_pcb *pcb  = data->pcb;

    // ── '*' heartbeat ─────────────────────────────────────────────────────────
    if (data->msg[0] == '*') {
        if (data->msg[1] == '+') heartbeatEnable[slot] = true;
        heartbeat[slot] = now_ms();
    }

    // ── 'N' device name ───────────────────────────────────────────────────────
    else if (data->msg[0] == 'N') {
        LOG_INFO(TAG, "N (device name) slot %d, heartbeat=%ds",
                 slot, heartbeatTimeout);
        char str[10];
        snprintf(str, sizeof(str), "*%d\n\n", heartbeatTimeout);
        send_msg(pcb, (int)strlen(str), str);
        tcp_output(pcb);
    }

    // ── 'H' device ID → envoyer accueil complet ───────────────────────────────
    else if (data->msg[0] == 'H') {
        LOG_INFO(TAG, "H (device ID) received → sending VN/HT/RL/PPA");
        tcp_write(pcb, "VN2.0\n", 6,  TCP_WRITE_FLAG_COPY);
        tcp_write(pcb, "HTWiBiDiB\n", 10, TCP_WRITE_FLAG_COPY);
        tcp_write(pcb, "RL0\n",   4,  TCP_WRITE_FLAG_COPY);
        tcp_write(pcb, "PPA2\n",  5,  TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
    }

    // ── 'P' power ────────────────────────────────────────────────────────────
    else if (data->msg[0] == 'P') {
        if (data->msg[1] == 'P' && data->msg[2] == 'A') {
            char stri[10];
            snprintf(stri, sizeof(stri), "PPA%c\n\n", data->msg[3]);
            send_msg(pcb, (int)strlen(stri), stri);
            tcp_output(pcb);
            // Traduire PPA en BiDiB boost
        if (data->msg[3] == '1') {
            bidib_send_boost_state(1);   // MSG_BOOST_ON
        } else if (data->msg[3] == '0') {
            bidib_send_boost_state(0);   // MSG_BOOST_OFF
        }
        // PPA2 = état inconnu → on ne fait rien
        }
    }

    // ── 'M' multithrottle ────────────────────────────────────────────────────
    else if (data->msg[0] == 'M') {
        printf("[withrottle_if] case M\n");

        char th[3] = {};
        th[0] = data->msg[1];  // '0' pour Engine Driver

        // Trouver ';' dans "M0+S14<;>S14"
        delimiter = 0;
        for (int j = 0; j < data->len; j++) {
            if (data->msg[j] == ';') { delimiter = j; break; }
        }

        // LocoAdress : msg[4..delimiter-2]
        memset(LocoAdress_g, 0, sizeof(LocoAdress_g));
        for (int k = 4; k < (delimiter - 1); k++)
            LocoAdress_g[k - 4] = data->msg[k];

        // Slot ED : index direct (1 loco par slot)
        Throttle_g       = slot;
        Loco[slot].slot  = slot;

        uint8_t action = (uint8_t)data->msg[2];  // '+', '-', 'A'

        // actionKey : après '<;>'
        memset(actionKey_g, 0, sizeof(actionKey_g));
        for (int k = (delimiter + 2); k < data->len; k++)
            actionKey_g[k - (delimiter + 2)] = data->msg[k];

        // Nettoyer \n / \r final
        int aklen = (int)strlen(actionKey_g);
        while (aklen > 0 && (actionKey_g[aklen-1] == '\n' ||
                              actionKey_g[aklen-1] == '\r'))
            actionKey_g[--aklen] = '\0';

        // ── '+' : ajout loco ──────────────────────────────────────────────────
        if (action == '+') {
            int k = (int)strtol(LocoAdress_g, NULL, 10);
            Loco[slot].dccAdress = k;
            printf("[withrottle_if] locoAdd th=%s ak=%s dcc=%d slot=%d\n",
                   th, actionKey_g, k, slot);
            locoAdd(th, actionKey_g, slot);
        }

        // ── '-' : release loco ────────────────────────────────────────────────
        else if (action == '-') {
            locoRelease(th, actionKey_g, slot);
        }

        // ── 'A' : action ──────────────────────────────────────────────────────
        else if (action == 'A') {
            locoAction(th, actionKey_g, slot);
        }

    } // case 'M'

    // ── 'Q' : client quitte ───────────────────────────────────────────────────
    else if (data->msg[0] == 'Q') {
        LOG_INFO(TAG, "Q: client quit, slot %d", slot);
        throttle_stop(slot);
    }

    // ── Vérification changement de vitesse ───────────────────────────────────
    if (Loco[slot].newSpeed != Loco[slot].oldSpeed) {
        Loco[slot].oldSpeed = Loco[slot].newSpeed;
        
        bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed, Loco[slot].dir, Loco[slot].LocoState);
    }

    // ── Heartbeat watchdog ────────────────────────────────────────────────────
    if (heartbeatEnable[slot]) checkHeartbeat(slot);
}