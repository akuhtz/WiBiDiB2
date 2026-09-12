#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Generally you would define your own explicit list of lwIP options
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html)
//
// This example uses a common include to avoid repetition
#include "lwipopts_examples_common.h"

// lwIP threaded mode (NO_SYS=0) requires mailboxes and thread sizes > 0
#ifndef TCPIP_MBOX_SIZE
#define TCPIP_MBOX_SIZE             16
#endif
#ifndef TCPIP_THREAD_STACKSIZE
#define TCPIP_THREAD_STACKSIZE      1024
#endif
#ifndef DEFAULT_THREAD_STACKSIZE
#define DEFAULT_THREAD_STACKSIZE    1024
#endif

// mDNS responder (Engine Driver discovery) — requires IGMP for IPv4 multicast
#ifndef LWIP_IGMP
#define LWIP_IGMP                1
#endif
#ifndef LWIP_MDNS_RESPONDER
#define LWIP_MDNS_RESPONDER      1
#endif
// mDNS stores per-netif data via lwIP client-data
#ifndef LWIP_NUM_NETIF_CLIENT_DATA
#define LWIP_NUM_NETIF_CLIENT_DATA  1
#endif
// mDNS (probe, announce, restart) ajoute des sys_timeout en plus du pool
// internal to lwIP → enlarge the pool to avoid PANIC
#ifndef MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT        16
#endif

#endif
