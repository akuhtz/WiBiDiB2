/*
 * flash_store.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Pilote SPI1 pour W25Q32VFSIG (SPI NOR, 4 MB).
 *
 * Remarque temps réel : les transferts SPI sont "bloquants" pour le CPU
 * mais n'effacent PAS les IRQ (spi_write/read_blocking sont des boucles
 * matérielles). Les ISR PIO BiDiB (priorité 0) s'exécutent donc pendant
 * toute lecture/écriture → le bus BiDiB continue d'être servi.
 *
 * Séquence d'écriture NOR :
 *   1. WRITE ENABLE (0x06)
 *   2. SECTOR ERASE (0x20) des secteurs modifiés
 *   3. attente WIP (READ STATUS 0x05, bit 0)
 *   4. PAGE PROGRAM (0x02), 256 octets max par commande
 *   5. attente WIP
 */

#include <string.h>

#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "flash_store.h"
#include "config.h"

static const char *TAG = "flash";

// ─── Commandes W25Q32 ─────────────────────────────────────────────────────────
#define W25_CMD_WRITE_ENABLE     0x06
#define W25_CMD_READ_STATUS      0x05
#define W25_CMD_READ_DATA        0x03
#define W25_CMD_PAGE_PROGRAM     0x02
#define W25_CMD_SECTOR_ERASE     0x20
#define W25_CMD_CHIP_ERASE       0xC7
#define W25_CMD_JEDEC_ID         0x9F

#define W25_STATUS_BUSY          0x01

// ─── Helpers bas niveau ───────────────────────────────────────────────────────
static void flash_cs_low(void) {
    gpio_put(FLASH_PIN_CS, 0);
}

static void flash_cs_high(void) {
    gpio_put(FLASH_PIN_CS, 1);
}

static void flash_write_enable(void) {
    uint8_t cmd = W25_CMD_WRITE_ENABLE;
    flash_cs_low();
    spi_write_blocking(FLASH_SPI, &cmd, 1);
    flash_cs_high();
}

static void flash_wait_ready(void) {
    uint8_t cmd = W25_CMD_READ_STATUS;
    uint8_t status = 0xFF;
    flash_cs_low();
    spi_write_blocking(FLASH_SPI, &cmd, 1);
    do {
        spi_read_blocking(FLASH_SPI, 0x00, &status, 1);
    } while (status & W25_STATUS_BUSY);
    flash_cs_high();
}

// ─── API publique ─────────────────────────────────────────────────────────────
uint32_t flash_store_jedec_id(void) {
    uint8_t cmd = W25_CMD_JEDEC_ID;
    uint8_t rx[3] = {0};
    flash_cs_low();
    spi_write_blocking(FLASH_SPI, &cmd, 1);
    spi_read_blocking(FLASH_SPI, 0x00, rx, 3);
    flash_cs_high();
    return ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
}

bool flash_store_init(void) {
    spi_init(FLASH_SPI, FLASH_SPI_FREQ_HZ);
    spi_set_format(FLASH_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_init(FLASH_PIN_SCK);
    gpio_set_function(FLASH_PIN_SCK, GPIO_FUNC_SPI);
    gpio_init(FLASH_PIN_MOSI);
    gpio_set_function(FLASH_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(FLASH_PIN_MISO);
    gpio_set_function(FLASH_PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(FLASH_PIN_CS);
    gpio_set_dir(FLASH_PIN_CS, GPIO_OUT);
    gpio_put(FLASH_PIN_CS, 1);

    uint32_t id = flash_store_jedec_id();
    if (id != 0x00EF4016) {               // Winbond W25Q32
        LOG_ERROR(TAG, "W25Q32 not detected (JEDEC ID 0x%06X)", id);
        return false;
    }
    LOG_INFO(TAG, "W25Q32 detected (JEDEC ID 0x%06X)", id);
    return true;
}

bool flash_store_read(uint32_t addr, uint8_t *buf, size_t len) {
    if (addr + len > FLASH_SIZE_BYTES) return false;

    uint8_t cmd[4] = { W25_CMD_READ_DATA,
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t)addr };
    flash_cs_low();
    spi_write_blocking(FLASH_SPI, cmd, 4);
    spi_read_blocking(FLASH_SPI, 0x00, buf, len);
    flash_cs_high();
    return true;
}

// Écrit `len` octets en préservant le contenu des secteurs non couverts.
// N'efface un secteur que si au moins un octet de la plage [addr, addr+len)
// le chevauche → écritures séquentielles bon marché.
bool flash_store_write(uint32_t addr, const uint8_t *buf, size_t len) {
    if (addr + len > FLASH_SIZE_BYTES) return false;
    if (len == 0) return true;

    static uint8_t sector[FLASH_SECTOR_SIZE];
    uint32_t end = addr + len;

    for (uint32_t a = addr; a < end; a += FLASH_SECTOR_SIZE) {
        uint32_t sector_base = a & ~(FLASH_SECTOR_SIZE - 1);
        uint32_t off_lo      = (a > sector_base)        ? a - sector_base : 0;
        uint32_t off_hi      = (end > sector_base + FLASH_SECTOR_SIZE)
                               ? FLASH_SECTOR_SIZE
                               : end - sector_base;

        if (!flash_store_read(sector_base, sector, FLASH_SECTOR_SIZE)) return false;

        memcpy(sector + off_lo, buf + (a - addr), off_hi - off_lo);

        flash_write_enable();
        uint8_t erase_cmd[4] = { W25_CMD_SECTOR_ERASE,
                                 (uint8_t)(sector_base >> 16),
                                 (uint8_t)(sector_base >> 8),
                                 (uint8_t)sector_base };
        flash_cs_low();
        spi_write_blocking(FLASH_SPI, erase_cmd, 4);
        flash_cs_high();
        flash_wait_ready();

        for (uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE) {
            flash_write_enable();
            uint8_t prog_cmd[4] = { W25_CMD_PAGE_PROGRAM,
                                    (uint8_t)((sector_base + p) >> 16),
                                    (uint8_t)((sector_base + p) >> 8),
                                    (uint8_t)(sector_base + p) };
            flash_cs_low();
            spi_write_blocking(FLASH_SPI, prog_cmd, 4);
            spi_write_blocking(FLASH_SPI, &sector[p], FLASH_PAGE_SIZE);
            flash_cs_high();
            flash_wait_ready();
        }
    }
    LOG_INFO(TAG, "wrote %u bytes @ 0x%06X", (unsigned)len, (unsigned)addr);
    return true;
}

bool flash_store_erase_all(void) {
    uint8_t cmd = W25_CMD_CHIP_ERASE;
    flash_write_enable();
    flash_cs_low();
    spi_write_blocking(FLASH_SPI, &cmd, 1);
    flash_cs_high();
    flash_wait_ready();
    LOG_INFO(TAG, "chip erased");
    return true;
}