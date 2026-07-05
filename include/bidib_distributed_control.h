/*! \file bidib_distributed_control.h
//========================================================================
//
// <b>OpenDCC - BiDiB - Guest-Messages</b>
//
// \brief Functions for upstream and downstream Guest-Messages
//
// You have to fill in the given templates to interact with basis firmware (FW).
//
// \copyright (c) 2021 Andreas Tillner
//
// This source file is subject of the GNU general public license 2, that is
// available at the world-wide-web at http://www.gnu.org/licenses/gpl.txt
//
//------------------------------------------------------------------------
//
// \author    Andreas Tillner
// \date      08.05.2021 19:00:00
//
// - contact:   a.tillner@gmx.de
// - website:   http://www.opendcc.de
//
// - history:   2021-05-08 V 0.00.01
//
//========================================================================*/

#ifndef ADDON_DISTRIBUTED_CONTROL_H_
#define ADDON_DISTRIBUTED_CONTROL_H_

#include "config.h"

//========================================================================
// Defines
//========================================================================
#define DEBUG_GUEST 1

// Class for MSG_GUEST_REQ_SUBSCRIBE / MSG_GATE_REQ_SUBSCRIBE
//#warning CLASS_REQ_xxx should be defined in bidib_message.h
#define CLASS_REQ_SYSTEM       0
#define CLASS_REQ_FEATURE      1
#define CLASS_REQ_OCCUOANCY    2
#define CLASS_REQ_BOOSTER      3
#define CLASS_REQ_ACCESSORY    4
#define CLASS_REQ_DCC_SIGNAL   6

// TARGET-Mode of a subscription message
//#warning BIDIB_TARGET_MODE_xxx should be defined in bidib_message.h
#define BIDIB_TARGET_MODE_ABSOLUTE              0
#define BIDIB_TARGET_MODE_RELATIVE1             1
//#define BIDIB_TARGET_MODE_TOP                  15   // defined in  bidib_messages.h
#define BIDIB_TARGET_MODE_DISPATCH_SWITCH      16
#define BIDIB_TARGET_MODE_DISPATCH_BOOSTER     17
#define BIDIB_TARGET_MODE_DISPATCH_ACCESSORY   18
#define BIDIB_TARGET_MODE_DISPATCH_DCCGEN      20
//#define BIDIB_TARGET_MODE_UID                  32  // defined in  bidib_messages.h

// Subscription ACK
//#warning SUBSCRIPTION_ACK_xxx should be defined in bidib_message.h
#define SUBSCRIPTION_ACK_OK             0x00
#define SUBSCRIPTION_ACK_CHANGED        0x01
#define SUBSCRIPTION_ACK_NO_SUPPORT     0x80
#define SUBSCRIPTION_ACK_TOO_MANY       0x81
#define SUBSCRIPTION_ACK_SELFORSUB      0x82
#define SUBSCRIPTION_ACK_NODE_NOT_EXIST 0xff

// SUB for MSG_GUEST_REQ_SUBSCRIBE / MSG_GATE_REQ_SUBSCRIBE
//#warning SUBSCRIPTION_REQ_xxx should be defined in bidib_message.h
#define SUBSCRIPTION_REQ_UPSTREAM    0
#define SUBSCRIPTION_REQ_DOWNSTREAM  1


//========================================================================
// Definitions
//========================================================================



/************************************************************************/
/************************************************************************/
/*                                                                      */
/*  This part handles the guest messages at a host (IF2 as localhost)   */
/*                                                                      */
/************************************************************************/
/************************************************************************/
#if (BIDIB_DISTRIBUTED_CONTROL == 1)

void AnalyseSubscription ( unsigned char idx, unsigned char rest, unsigned char* msg);

#endif // (BIDIB_DISTRIBUTED_CONTROL == 1)
/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/



/************************************************************************/
/************************************************************************/
/*                                                                      */
/*  This part handles the guest messages at the guest                   */
/*                                                                      */
/************************************************************************/
/************************************************************************/
#if (CLASS_GUEST == 1)

#define CS_STATE_DIR_XP      1
#define CS_STATE_DIR_BIDIB   2

uint8_t g_bidib_guest_enabled;   // 1 = guest messages enabled, 0 = guest messages disabled
#define is_guest_msg_enabled()    g_bidib_guest_enabled
#define set_guest_msg_enabled()   g_bidib_guest_enabled = 1
#define reset_guest_msg_enabled() g_bidib_guest_enabled = 0


// Functions
void checkIfGuestMsgAreDisabled (uint8_t rest, unsigned char *msg);
void checkIfGuestMsgAreEnabled (uint8_t rest, unsigned char *msg);
bool guest_req_subscribe_host (uint8_t class, uint8_t sub, uint8_t objCount);
void guest_resp_subscribtion_parser (uint8_t, unsigned char *);
void guest_resp_fyi_parser (uint8_t, unsigned char *);
bool guest_req_send (uint8_t msgType, uint8_t TargetMode, uint8_t* msgData, uint8_t dataLen);
void am_get_parser(unsigned char* msg);
void am_set_parser(unsigned char* msg);

#if (GUEST_ASPECT_LIST == 1)   // defines and functions to parse MSG_VENDOR_SET/GET for Accessory mapping table



#endif // (GUEST_ASPECT_LIST == 1)

#endif // (CLASS_GUEST == 1)
/************************************************************************/
/************************************************************************/
/************************************************************************/
/************************************************************************/

#endif /* ADDON_DISTRIBUTED_CONTROL_H_ */
