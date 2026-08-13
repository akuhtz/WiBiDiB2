/*
 * log.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Logging non-bloquant : LOG_* écrit dans un ring buffer RAM, log_poll()
 * draine le buffer vers l'UART (bridge debug probe) sans bloquer la boucle principale.
 *
 * Pierre Moulin
 */

#ifndef LOG_H_
#define LOG_H_

#include <stdint.h>
#include <stdarg.h>

#ifndef LOG_RING_SIZE
#define LOG_RING_SIZE 2048
#endif

void    log_init(void);
void    log_printf(const char *fmt, ...);
void    log_poll(void);

#endif /* LOG_H_ */