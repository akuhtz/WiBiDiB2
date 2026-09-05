/*
 * withrottle_if.h  —  Pico 2W
 */
#ifndef WITHROTTLE_IF_H_
#define WITHROTTLE_IF_H_

#include "datatypes.h"

// Processing a received WiThrottle message
// slot = index in throttle[] (passed directly from tcp_server_recv_cb)
void process_rx_withrottle(rx_data_t *data, uint8_t slot);

// Emergency stop of a throttle (called on disconnect)
void throttle_stop(uint8_t slot);

#endif /* WITHROTTLE_IF_H_ */