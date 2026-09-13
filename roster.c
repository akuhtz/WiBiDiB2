/*
 * roster.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Flash-backed roster with 4-slot LRU cache in RAM.
 *
 * Storage layout (in W25Q32 external flash):
 *   ROSTER_HEADER_ADDR  (0x001000)  header sector — magic, version, bitmap, CRC
 *   ROSTER_ENTRY_BASE   (0x002000)  slot 0 sector
 *                       + N*STRIDE  slot N sector
 *
 * Cache lookup: linear scan of 4 slots (O(1) constant).
 * Cache miss:   read the entry sector from flash (~1 ms), evict LRU slot.
 *
 * All public API is thread-safe via an internal recursive mutex.
 * The mutex is NEVER held across flash write operations — flash writes
 * are dispatched to the roster_writer task via a queue.
 */

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "roster.h"
#include "roster_writer.h"
#include "flash_store.h"
#include "config.h"

static const char *TAG = "roster";

// ─── Cache state ─────────────────────────────────────────────────────────
typedef struct {
    uint8_t         slot;        // physical flash slot, or ROSTER_SLOT_INVALID
    uint32_t        lru_stamp;   // higher = more recently used
    roster_entry_t  entry;
} cache_slot_t;

static cache_slot_t      cache[ROSTER_CACHE_SIZE];
static uint32_t          lru_counter = 0;
static uint32_t          used_bitmap = 0;    // bit N = slot N used
static uint8_t           count_cached = 0;   // popcount(used_bitmap)
static SemaphoreHandle_t mutex = NULL;

// Shared scratch buffer for internal lookups. Safe because it is ONLY
// accessed with the mutex held, and only one task holds the mutex at a
// time. Keeps ~1.2 KB off the caller's stack.
static roster_entry_t    scratch;

// ─── Header struct (packed for flash storage) ────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t used_bitmap;
    uint32_t crc32;
} roster_header_t;

// ─── CRC32 (IEEE 802.3 poly, matches zlib.crc32) ─────────────────────────
static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// ─── Utilities ───────────────────────────────────────────────────────────
static uint32_t entry_addr(uint8_t slot) {
    return ROSTER_ENTRY_BASE + (uint32_t)slot * ROSTER_ENTRY_STRIDE;
}

static uint8_t popcount32(uint32_t x) {
    uint8_t n = 0;
    while (x) { n += (x & 1u); x >>= 1; }
    return n;
}

// ─── Cache management (assumes mutex held) ───────────────────────────────
static cache_slot_t *cache_find(uint8_t slot) {
    for (int i = 0; i < ROSTER_CACHE_SIZE; i++) {
        if (cache[i].slot == slot) return &cache[i];
    }
    return NULL;
}

static cache_slot_t *cache_pick_lru(void) {
    cache_slot_t *victim = &cache[0];
    for (int i = 1; i < ROSTER_CACHE_SIZE; i++) {
        if (cache[i].slot == ROSTER_SLOT_INVALID) return &cache[i];
        if (cache[i].lru_stamp < victim->lru_stamp) victim = &cache[i];
    }
    return victim;
}

static void cache_invalidate(uint8_t slot) {
    cache_slot_t *c = cache_find(slot);
    if (c) c->slot = ROSTER_SLOT_INVALID;
}

// Fetch a slot from flash into a caller buffer. Also fills a cache slot.
// Returns false on flash read error. Assumes mutex held.
static bool cache_fetch(uint8_t slot, roster_entry_t *out) {
    cache_slot_t *c = cache_find(slot);
    if (c) {
        c->lru_stamp = ++lru_counter;
        memcpy(out, &c->entry, sizeof(*out));
        return true;
    }
    // miss — read from flash into an LRU-picked cache slot
    c = cache_pick_lru();
    if (!flash_store_read(entry_addr(slot), (uint8_t *)&c->entry, sizeof(c->entry))) {
        c->slot = ROSTER_SLOT_INVALID;
        return false;
    }
    c->slot      = slot;
    c->lru_stamp = ++lru_counter;
    memcpy(out, &c->entry, sizeof(*out));
    return true;
}

// Update an in-cache copy (or bring it in and update). Assumes mutex held.
static void cache_put(uint8_t slot, const roster_entry_t *in) {
    cache_slot_t *c = cache_find(slot);
    if (!c) c = cache_pick_lru();
    memcpy(&c->entry, in, sizeof(*in));
    c->slot      = slot;
    c->lru_stamp = ++lru_counter;
}

// ─── Header IO ───────────────────────────────────────────────────────────
static bool header_read(roster_header_t *hdr) {
    if (!flash_store_read(ROSTER_HEADER_ADDR, (uint8_t *)hdr, sizeof(*hdr))) return false;
    if (hdr->magic != ROSTER_HEADER_MAGIC) return false;
    if (hdr->version != ROSTER_HEADER_VERSION) return false;
    uint32_t want = crc32_calc((const uint8_t *)hdr, sizeof(*hdr) - sizeof(hdr->crc32));
    return hdr->crc32 == want;
}

static bool header_write(uint32_t bitmap) {
    roster_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = ROSTER_HEADER_MAGIC;
    hdr.version     = ROSTER_HEADER_VERSION;
    hdr.reserved    = 0;
    hdr.used_bitmap = bitmap;
    hdr.crc32       = crc32_calc((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.crc32));
    return flash_store_write(ROSTER_HEADER_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
}

// ─── Bootstrap defaults (first-boot, blank flash) ────────────────────────
static void bootstrap_fill_entry(roster_entry_t *e,
                                 const char *id, const char *roadName,
                                 const char *roadNumber, const char *mfg,
                                 const char *model, const char *decoderModel,
                                 const char *decoderFamily,
                                 uint16_t dccAddr, bool longAddr,
                                 uint8_t maxSpeed, uint8_t maxFnNum) {
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, ROSTER_STRING_MAX - 1);
    strncpy(e->roadName, roadName, ROSTER_STRING_MAX - 1);
    strncpy(e->roadNumber, roadNumber, ROSTER_STRING_MAX - 1);
    strncpy(e->mfg, mfg, ROSTER_STRING_MAX - 1);
    strncpy(e->model, model, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderModel, decoderModel, ROSTER_STRING_MAX - 1);
    strncpy(e->decoderFamily, decoderFamily, ROSTER_STRING_MAX - 1);
    e->dccAddress  = dccAddr;
    e->longAddress = longAddr;
    e->maxSpeed    = maxSpeed;
    e->maxFnNum    = maxFnNum;
}

static void bootstrap_set_func(roster_entry_t *e, uint8_t fn,
                               bool lockable, bool visible, const char *label) {
    if (fn > ROSTER_FUNC_MAX) return;
    e->functions[fn].lockable = lockable;
    e->functions[fn].visible  = visible;
    strncpy(e->functions[fn].label, label, ROSTER_STRING_MAX - 1);
}

// Write the three default entries directly to flash + a fresh header.
// Called with mutex held.
static bool bootstrap_defaults(void) {
    LOG_INFO(TAG, "Blank/invalid roster header — writing defaults");
    roster_entry_t e;

    // ── Slot 0 ────────────────────────────────────────────────────────
    bootstrap_fill_entry(&e, "BR 185 203-1", "DB", "185 203-1", "TRIX",
                         "TRAXX F140 AC", "ESU LokSound 5", "ESU LokSound 5",
                         6, false, 100, 6);
    bootstrap_set_func(&e, 0, true,  true, "Headlight");
    bootstrap_set_func(&e, 1, true,  true, "Bell");
    bootstrap_set_func(&e, 2, false, true, "Horn");
    bootstrap_set_func(&e, 3, true,  true, "Compressor");
    bootstrap_set_func(&e, 4, true,  true, "Cab Light");
    bootstrap_set_func(&e, 5, true,  true, "Ditch Lights");
    if (!flash_store_write(entry_addr(0), (const uint8_t *)&e, sizeof(e))) return false;

    // ── Slot 1 ────────────────────────────────────────────────────────
    bootstrap_fill_entry(&e, "BR 01 1066-9", "DB", "01 1066-9", "ROCO",
                         "BR 01", "ESU LokSound 5", "ESU LokSound 5",
                         1, false, 100, 8);
    bootstrap_set_func(&e, 0, true,  true, "Headlight");
    bootstrap_set_func(&e, 1, false, true, "Bell");
    bootstrap_set_func(&e, 2, false, true, "Whistle");
    bootstrap_set_func(&e, 3, true,  true, "Steam Blowoff");
    bootstrap_set_func(&e, 4, true,  true, "Safety Valve");
    bootstrap_set_func(&e, 5, true,  true, "Oil Cup");
    bootstrap_set_func(&e, 6, true,  true, "Generator");
    bootstrap_set_func(&e, 7, true,  true, "Cab Light");
    if (!flash_store_write(entry_addr(1), (const uint8_t *)&e, sizeof(e))) return false;

    // ── Slot 2 ────────────────────────────────────────────────────────
    bootstrap_fill_entry(&e, "UP 4014", "Union Pacific", "4014", "Athearn",
                         "DD40AX Big Boy", "NCE DCC Gold", "NCE",
                         4014, true, 100, 6);
    bootstrap_set_func(&e, 0, true,  true, "Headlight");
    bootstrap_set_func(&e, 1, true,  true, "Bell");
    bootstrap_set_func(&e, 2, false, true, "Horn");
    bootstrap_set_func(&e, 3, true,  true, "Ditch Lights");
    bootstrap_set_func(&e, 4, true,  true, "Cab Light");
    bootstrap_set_func(&e, 5, true,  true, "Dynamic Brakes");
    if (!flash_store_write(entry_addr(2), (const uint8_t *)&e, sizeof(e))) return false;

    uint32_t bitmap = 0x00000007u;   // slots 0,1,2
    if (!header_write(bitmap)) return false;
    used_bitmap  = bitmap;
    count_cached = 3;
    return true;
}

// ─── Public API ──────────────────────────────────────────────────────────
bool roster_init(void) {
    if (mutex == NULL) {
        mutex = xSemaphoreCreateRecursiveMutex();
        if (!mutex) return false;
    }
    for (int i = 0; i < ROSTER_CACHE_SIZE; i++) {
        cache[i].slot      = ROSTER_SLOT_INVALID;
        cache[i].lru_stamp = 0;
    }

    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    roster_header_t hdr;
    if (header_read(&hdr)) {
        used_bitmap  = hdr.used_bitmap;
        count_cached = popcount32(used_bitmap);
        LOG_INFO(TAG, "Header OK: %u entries (bitmap 0x%08lX)",
                 count_cached, (unsigned long)used_bitmap);
    } else {
        if (!bootstrap_defaults()) {
            LOG_ERROR(TAG, "Failed to write default roster");
            xSemaphoreGiveRecursive(mutex);
            return false;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return true;
}

bool roster_get(uint8_t slot, roster_entry_t *out) {
    if (slot >= ROSTER_MAX_ENTRIES || !out) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    bool ok = false;
    if (used_bitmap & (1u << slot)) {
        ok = cache_fetch(slot, out);
    }
    xSemaphoreGiveRecursive(mutex);
    return ok;
}

bool roster_get_dense(uint8_t dense_index, uint8_t *out_slot, roster_entry_t *out) {
    if (!out_slot || !out) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    bool ok = false;
    uint8_t seen = 0;
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (!(used_bitmap & (1u << s))) continue;
        if (seen == dense_index) {
            *out_slot = s;
            ok = cache_fetch(s, out);
            break;
        }
        seen++;
    }
    xSemaphoreGiveRecursive(mutex);
    return ok;
}

bool roster_find_by_addr(uint16_t dcc, roster_entry_t *out) {
    if (!out) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    bool ok = false;
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (!(used_bitmap & (1u << s))) continue;
        if (!cache_fetch(s, &scratch)) continue;
        if (scratch.dccAddress == dcc) {
            memcpy(out, &scratch, sizeof(*out));
            ok = true;
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return ok;
}

bool roster_find_by_id(const char *id, uint8_t *out_slot) {
    if (!id || !out_slot) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    bool ok = false;
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (!(used_bitmap & (1u << s))) continue;
        if (!cache_fetch(s, &scratch)) continue;
        if (strncmp(scratch.id, id, ROSTER_STRING_MAX) == 0) {
            *out_slot = s;
            ok = true;
            break;
        }
    }
    xSemaphoreGiveRecursive(mutex);
    return ok;
}

uint8_t roster_count_valid(void) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    uint8_t n = count_cached;
    xSemaphoreGiveRecursive(mutex);
    return n;
}

// ─── Mutations (Step 1: synchronous flash write, will move to writer task) ──
// These update the cache + shadow bitmap under the mutex, then enqueue a
// job to the writer task. The writer task performs the flash IO
// asynchronously (see roster_writer.c). If the queue is not yet
// initialized (init-time bootstrap), we fall back to a direct write.

static bool dispatch_write(uint8_t slot, const roster_entry_t *e, uint32_t new_bitmap) {
    roster_write_job_t job = {
        .op         = RW_OP_WRITE_ENTRY,
        .slot       = slot,
        .new_bitmap = new_bitmap,
    };
    memcpy(&job.entry, e, sizeof(*e));
    return roster_writer_enqueue(&job);
}

static bool dispatch_erase(uint8_t slot, uint32_t new_bitmap) {
    roster_write_job_t job = {
        .op         = RW_OP_ERASE_ENTRY,
        .slot       = slot,
        .new_bitmap = new_bitmap,
    };
    return roster_writer_enqueue(&job);
}

bool roster_add(const roster_entry_t *in, uint8_t *out_slot) {
    if (!in || !out_slot) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    // Duplicate-id check
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (!(used_bitmap & (1u << s))) continue;
        if (cache_fetch(s, &scratch) &&
            strncmp(scratch.id, in->id, ROSTER_STRING_MAX) == 0) {
            xSemaphoreGiveRecursive(mutex);
            LOG_WARN(TAG, "roster_add: duplicate id '%s'", in->id);
            return false;
        }
    }

    // Find lowest free slot
    uint8_t slot = ROSTER_SLOT_INVALID;
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (!(used_bitmap & (1u << s))) { slot = s; break; }
    }
    if (slot == ROSTER_SLOT_INVALID) {
        xSemaphoreGiveRecursive(mutex);
        LOG_WARN(TAG, "roster_add: roster is full");
        return false;
    }

    uint32_t new_bitmap = used_bitmap | (1u << slot);
    cache_put(slot, in);
    used_bitmap  = new_bitmap;
    count_cached = popcount32(new_bitmap);
    *out_slot    = slot;

    xSemaphoreGiveRecursive(mutex);
    return dispatch_write(slot, in, new_bitmap);
}

bool roster_update(uint8_t slot, const roster_entry_t *in) {
    if (!in || slot >= ROSTER_MAX_ENTRIES) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (!(used_bitmap & (1u << slot))) {
        xSemaphoreGiveRecursive(mutex);
        LOG_WARN(TAG, "roster_update: slot %u not in use", slot);
        return false;
    }

    // Duplicate-id check (only if some OTHER slot has the same id)
    for (uint8_t s = 0; s < ROSTER_MAX_ENTRIES; s++) {
        if (s == slot || !(used_bitmap & (1u << s))) continue;
        if (cache_fetch(s, &scratch) &&
            strncmp(scratch.id, in->id, ROSTER_STRING_MAX) == 0) {
            xSemaphoreGiveRecursive(mutex);
            LOG_WARN(TAG, "roster_update: duplicate id '%s' in slot %u", in->id, s);
            return false;
        }
    }

    cache_put(slot, in);
    xSemaphoreGiveRecursive(mutex);
    return dispatch_write(slot, in, used_bitmap);
}

bool roster_delete(uint8_t slot) {
    if (slot >= ROSTER_MAX_ENTRIES) return false;
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    if (!(used_bitmap & (1u << slot))) {
        xSemaphoreGiveRecursive(mutex);
        return false;
    }

    uint32_t new_bitmap = used_bitmap & ~(1u << slot);
    cache_invalidate(slot);
    used_bitmap  = new_bitmap;
    count_cached = popcount32(new_bitmap);

    xSemaphoreGiveRecursive(mutex);
    return dispatch_erase(slot, new_bitmap);
}

// ─── Rollback hook (called by writer task on flash failure) ──────────────
// Re-reads the entry sector and the header from flash and restores the
// cache + shadow bitmap to match. Called with mutex held internally.
void roster_rollback_from_flash(uint8_t slot) {
    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    roster_header_t hdr;
    if (header_read(&hdr)) {
        used_bitmap  = hdr.used_bitmap;
        count_cached = popcount32(used_bitmap);
    }
    cache_invalidate(slot);   // force a re-read on next access
    xSemaphoreGiveRecursive(mutex);
}
