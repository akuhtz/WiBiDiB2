/*
 * roster.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Flash-backed roster with small in-RAM LRU cache.
 * Full roster (up to ROSTER_MAX_ENTRIES entries) lives in external
 * W25Q32 flash. Only ROSTER_CACHE_SIZE entries are held in RAM at a
 * time. All public accessors copy the entry into a caller buffer —
 * pointers into the cache are NOT valid across API calls (an entry
 * may be evicted).
 *
 * Thread-safety: all public functions take an internal recursive mutex.
 * Callers must not call flash_store_* directly for roster data.
 */
#ifndef ROSTER_H_
#define ROSTER_H_

#include <stdint.h>
#include <stdbool.h>

#define ROSTER_MAX_ENTRIES    32          // physical slots in flash
#define ROSTER_CACHE_SIZE     4           // in-RAM LRU cache slots
#define ROSTER_FUNC_MAX       28
#define ROSTER_STRING_MAX     32
#define ROSTER_SLOT_INVALID   0xFF

typedef struct {
    char     id[ROSTER_STRING_MAX];
    char     roadName[ROSTER_STRING_MAX];
    char     roadNumber[ROSTER_STRING_MAX];
    char     mfg[ROSTER_STRING_MAX];
    char     model[ROSTER_STRING_MAX];
    char     decoderModel[ROSTER_STRING_MAX];
    char     decoderFamily[ROSTER_STRING_MAX];
    uint16_t dccAddress;
    bool     longAddress;
    uint8_t  maxSpeed;
    uint8_t  maxFnNum;
    struct {
        bool  lockable;
        bool  visible;
        char  label[ROSTER_STRING_MAX];
    } functions[ROSTER_FUNC_MAX + 1];
} roster_entry_t;

// Reads the flash header, primes state. On bad header (blank flash or
// bad CRC/magic) writes a default roster (BR 185, BR 01, UP 4014) to flash.
// Must be called after flash_store_init() and after the scheduler starts
// (uses FreeRTOS mutex primitives).
bool roster_init(void);

// Get by physical slot (0..ROSTER_MAX_ENTRIES-1). Returns false if slot is
// unused, out of range, or a flash read failed.
bool roster_get(uint8_t slot, roster_entry_t *out);

// Dense iteration: maps 0..count-1 to used slot numbers in ascending order.
// Returns the physical slot in *out_slot (may be non-contiguous).
bool roster_get_dense(uint8_t dense_index, uint8_t *out_slot, roster_entry_t *out);

// Lookup by DCC address. Returns false if no entry matches.
bool roster_find_by_addr(uint16_t dcc, roster_entry_t *out);

// Lookup by id string. Returns physical slot number in *out_slot.
// Returns false if no entry matches.
bool roster_find_by_id(const char *id, uint8_t *out_slot);

// Add a new entry in the lowest-numbered free slot. Fails (returns false)
// if the roster is full OR if an entry with the same id already exists.
// On success, *out_slot receives the assigned slot number.
bool roster_add(const roster_entry_t *in, uint8_t *out_slot);

// Overwrite an existing entry. Fails if slot is unused or out of range.
// Duplicate-id check: fails if some OTHER slot has the same id.
bool roster_update(uint8_t slot, const roster_entry_t *in);

// Delete an entry. Fails if slot is unused or out of range.
bool roster_delete(uint8_t slot);

// Number of used slots (popcount of the used bitmap).
uint8_t roster_count_valid(void);

#endif /* ROSTER_H_ */
