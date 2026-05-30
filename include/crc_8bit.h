/*
    CRC-8 for Dallas iButton products

    From Maxim/Dallas AP Note 27

    "Understanding and Using Cyclic Redundancy Checks with
    Dallas Semiconductor iButton Products"

    The Ap note describes the CRC-8 algorithm used in the
    iButton products. Their implementation involves a 256 byte
    CRC table. This algorithm is implemented here. In addition
    two other algorithms are shown. One uses nibble arrays and
    the other uses boolean arithmetic.

    18JAN03 - T. Scott Dattalo

  crc array from the Maxim ApNote
*/

#include <stdint.h>

#ifndef MAIN_INCLUDE_CRC_8BIT_H_
#define MAIN_INCLUDE_CRC_8BIT_H_

#define read_crc_array(index)  crc_array[index]
extern const unsigned char crc_array[256];

static inline uint8_t crc8_update(uint8_t crc, uint8_t data) {
    return crc_array[crc ^ data];
}

#endif /* MAIN_INCLUDE_CRC_8BIT_H_ */
