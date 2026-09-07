/*
 * http_server.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Minimal HTTP server for EngineDriver roster access.
 * Serves roster XML at GET /roster/?format=xml
 */
#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_

#include <stdbool.h>

bool http_server_init(void);

#endif /* HTTP_SERVER_H_ */
