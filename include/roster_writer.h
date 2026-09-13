/*
 * roster_writer.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Background flash writer for roster entries. The writer runs in its own
 * FreeRTOS task at priority 1 so that the ~200 ms SPI stall of a
 * flash sector erase+program does not delay the BiDiB parser, WiFi,
 * or lwIP tcpip thread.
 *
 * On flash-write failure: log an error, roll back the affected cache
 * slot from flash (so the RAM state matches what actually persists),
 * and put the LED in the persistent LED_ERROR state.
 */
#ifndef ROSTER_WRITER_H_
#define ROSTER_WRITER_H_

#include <stdint.h>
#include <stdbool.h>
#include "roster.h"

typedef enum {
    RW_OP_WRITE_ENTRY,    // erase + program a single entry sector + rewrite header
    RW_OP_ERASE_ENTRY,    // erase entry sector + rewrite header
} roster_write_op_t;

typedef struct {
    roster_write_op_t op;
    uint8_t           slot;
    uint32_t          new_bitmap;
    roster_entry_t    entry;      // only meaningful for RW_OP_WRITE_ENTRY
} roster_write_job_t;

// Creates the writer task and its queue. Call once before vTaskStartScheduler().
bool roster_writer_init(void);

// Enqueue a job. Blocks briefly if the queue is full (max 2 jobs).
bool roster_writer_enqueue(const roster_write_job_t *job);

// Called internally by the writer on flash failure to restore consistency.
// Public here so roster.c can call it too if needed for tests.
void roster_rollback_from_flash(uint8_t slot);

#endif /* ROSTER_WRITER_H_ */
