/*
 * features.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Définition des features BiDiB supportés par le gateway
 * (MSG_FEATURE_GETALL / GETNEXT / GET / SET).
 * Le handling (get_feature/set_feature/...) vit dans bidib_client_parser.c.
 *
 * Pierre Moulin
 */

#ifndef FEATURES_H_
#define FEATURES_H_

#include <stdint.h>

#include "bidib_messages.h"
#include "config.h"

typedef struct {
    uint8_t num;    // numéro de feature BiDiB
    uint8_t value;  // valeur courante
    uint8_t min;    // borne min
    uint8_t max;    // borne max
} wb_feature_t;

enum { NUM_OF_FEATURES = 4 };

static wb_feature_t g_features[NUM_OF_FEATURES] = {
    { FEATURE_STRING_SIZE,       BIDIB_STRING_MAX, BIDIB_STRING_MAX, BIDIB_STRING_MAX }, // 252
    { FEATURE_STRING_DEBUG,      0,                0,                1 },                 // 251
    { FEATURE_FW_UPDATE_MODE,    0,                0,                1 },                 // 254
    { FEATURE_RELEVANT_PID_BITS, 16,               1,                16 },                // 253
};

#endif /* FEATURES_H_ */
