/*
 * datatypes.h  —  WiBiDiB  (Pico 2W)
 */
#ifndef DATATYPES_H_
#define DATATYPES_H_

#include <stdint.h>
#include "config.h"
#include "lwip/tcp.h"

typedef enum {
    NODE_INACTIVE  = 0,
    NODE_LOGGED_ON = 1,
    NODE_ACTIVE    = 2,
} t_node_state;

typedef struct {
    t_node_state    state;
    struct tcp_pcb *pcb;       // NULL = slot libre
    uint8_t         node_adr;
} throttle_t;

typedef struct {
    struct tcp_pcb *pcb;
    int             len;
    char            msg[128];
} rx_data_t;

extern throttle_t throttle[MAX_THROTTLES];
extern uint8_t    cli_index;

uint8_t find_throttle_by_pcb(struct tcp_pcb *pcb);
uint8_t find_free_throttle_slot(void);

#endif