/*
 * smartphone_if.h  —  Pico 2W
 */
#ifndef SMARTPHONE_IF_H_
#define SMARTPHONE_IF_H_

#include <stdint.h>

void smartphone_if_init(void);

uint8_t find_throttle_by_pcb(struct tcp_pcb *pcb);
uint8_t find_free_throttle_slot(void);

#endif /* SMARTPHONE_IF_H_ */