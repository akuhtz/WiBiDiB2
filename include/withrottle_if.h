/*
 * withrottle_if.h  —  Pico 2W
 */
#ifndef WITHROTTLE_IF_H_
#define WITHROTTLE_IF_H_

#include "datatypes.h"

// Send welcome sequence (VN/HT/Ht/RL/PPA) to a newly connected client
void send_welcome_message(struct tcp_pcb *pcb);

// Processing a received WiThrottle message
// slot = index in throttle[] (passed directly from tcp_server_recv_cb)
void process_rx_withrottle(rx_data_t *data, uint8_t slot);

// Emergency stop of a throttle (called on disconnect)
void throttle_stop(uint8_t slot);

#endif /* WITHROTTLE_IF_H_ */