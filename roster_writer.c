/*
 * roster_writer.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Background flash writer task. Serializes all writes to the roster
 * flash region (entry sectors + header sector) so the ~200 ms SPI
 * stall does not block lwIP callbacks, WiThrottle handling, or BiDiB.
 *
 * On any flash write failure:
 *   1. LOG_ERROR
 *   2. Roll back the cache from flash (RAM state == last persisted state)
 *   3. led_set_state(LED_ERROR) — persistent visual indicator
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "roster_writer.h"
#include "roster.h"
#include "flash_store.h"
#include "led.h"
#include "config.h"

static const char *TAG = "roster_wr";

#define ROSTER_WRITER_QUEUE_LEN     2
#define ROSTER_WRITER_STACK_WORDS   1024
#define ROSTER_WRITER_PRIORITY      1

static QueueHandle_t write_queue = NULL;

static uint32_t entry_addr(uint8_t slot) {
    return ROSTER_ENTRY_BASE + (uint32_t)slot * ROSTER_ENTRY_STRIDE;
}

// ─── CRC32 (IEEE 802.3 poly) — duplicated for isolation from roster.c ───
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

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t used_bitmap;
    uint32_t crc32;
} roster_header_t;

static bool write_header(uint32_t bitmap) {
    roster_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = ROSTER_HEADER_MAGIC;
    hdr.version     = ROSTER_HEADER_VERSION;
    hdr.reserved    = 0;
    hdr.used_bitmap = bitmap;
    hdr.crc32       = crc32_calc((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.crc32));
    return flash_store_write(ROSTER_HEADER_ADDR, (const uint8_t *)&hdr, sizeof(hdr));
}

// ─── Job execution ───────────────────────────────────────────────────────
#ifdef ROSTER_INJECT_FLASH_FAIL
// Debug: fail every Nth flash write. Set ROSTER_INJECT_FLASH_FAIL to a
// small positive number at compile time to exercise the rollback path.
static int inject_counter = 0;
static bool inject_should_fail(void) {
    if (++inject_counter >= ROSTER_INJECT_FLASH_FAIL) {
        inject_counter = 0;
        return true;
    }
    return false;
}
#else
static inline bool inject_should_fail(void) { return false; }
#endif

static bool execute_job(const roster_write_job_t *job) {
    switch (job->op) {
    case RW_OP_WRITE_ENTRY:
        LOG_INFO(TAG, "Writing entry slot %u", job->slot);
        if (inject_should_fail() ||
            !flash_store_write(entry_addr(job->slot),
                               (const uint8_t *)&job->entry,
                               sizeof(job->entry))) {
            LOG_ERROR(TAG, "flash_store_write failed for slot %u", job->slot);
            return false;
        }
        if (!write_header(job->new_bitmap)) {
            LOG_ERROR(TAG, "header write failed after slot %u", job->slot);
            return false;
        }
        return true;

    case RW_OP_ERASE_ENTRY:
        LOG_INFO(TAG, "Erasing entry slot %u", job->slot);
        {
            uint8_t blank[sizeof(roster_entry_t)];
            memset(blank, 0xFF, sizeof(blank));
            if (inject_should_fail() ||
                !flash_store_write(entry_addr(job->slot), blank, sizeof(blank))) {
                LOG_ERROR(TAG, "flash erase failed for slot %u", job->slot);
                return false;
            }
        }
        if (!write_header(job->new_bitmap)) {
            LOG_ERROR(TAG, "header write failed after erase slot %u", job->slot);
            return false;
        }
        return true;
    }
    return false;
}

// ─── Task ────────────────────────────────────────────────────────────────
static void roster_writer_task(void *param) {
    (void)param;
    roster_write_job_t job;
    for (;;) {
        if (xQueueReceive(write_queue, &job, portMAX_DELAY) == pdTRUE) {
            if (!execute_job(&job)) {
                // Roll back the affected slot in the cache so RAM matches flash.
                roster_rollback_from_flash(job.slot);
                led_set_state(LED_ERROR);
            }
        }
    }
}

bool roster_writer_init(void) {
    if (write_queue) return true;
    write_queue = xQueueCreate(ROSTER_WRITER_QUEUE_LEN, sizeof(roster_write_job_t));
    if (!write_queue) return false;
    return xTaskCreate(roster_writer_task, "roster_wr",
                       ROSTER_WRITER_STACK_WORDS, NULL,
                       ROSTER_WRITER_PRIORITY, NULL) == pdPASS;
}

bool roster_writer_enqueue(const roster_write_job_t *job) {
    if (!write_queue) {
        // Called before the writer task exists (e.g. during roster_init
        // bootstrap). Fall back to synchronous execution — this only
        // happens for the initial defaults write.
        return execute_job(job);
    }
    return xQueueSend(write_queue, job, pdMS_TO_TICKS(1000)) == pdPASS;
}
