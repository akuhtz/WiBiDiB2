/*
 * flash_store.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Pilote SPI pour la mémoire flash externe W25Q32VFSIG (32 Mbit = 4 MB).
 * Connectée sur SPI1 (indépendante du PIO0 utilisé par le bus BiDiB).
 *
 * Les transferts SPI bloquent le CPU, mais PAS les IRQ : les ISR PIO
 * BiDiB (bidib_pio_rx_isr/tx_isr, priorité 0) continuent de tourner
 * pendant une écriture → le trafic BiDiB n'est pas interrompu.
 *
 * Pins (à raccorder) :
 *   GP10 = SCK  (SPI1)
 *   GP11 = MOSI (TX)
 *   GP12 = MISO (RX)
 *   GP13 = CS   (chip select, piloté en GPIO)
 */

#ifndef FLASH_STORE_H_
#define FLASH_STORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ─── Configuration matérielle (SPI1) ─────────────────────────────────────────
#define FLASH_SPI            spi1
#define FLASH_PIN_SCK        10
#define FLASH_PIN_MOSI       11
#define FLASH_PIN_MISO       12
#define FLASH_PIN_CS         13
#define FLASH_SPI_FREQ_HZ    2000000      // 2 MHz, conservateur

// W25Q32VFSIG : 32 Mbit = 4 MByte, secteurs de 4 KByte, pages de 256 octets
#define FLASH_SIZE_BYTES     (4 * 1024 * 1024)
#define FLASH_SECTOR_SIZE    4096
#define FLASH_PAGE_SIZE      256

// Initialise SPI1 + GPIO CS. Retourne true si la JEDEC ID est correcte
// (manufacturer 0xEF = Winbond, device 0x4015 = W25Q32).
bool flash_store_init(void);

// Retourne l'ID JEDEC : 0x00EF4015 pour un W25Q32 (ou 0 si absent).
uint32_t flash_store_jedec_id(void);

// Lit `len` octets depuis `addr` (0x000000 .. FLASH_SIZE_BYTES-1).
// Retourne false si hors bornes.
bool flash_store_read(uint32_t addr, uint8_t *buf, size_t len);

// Écrit `len` octets à `addr`. Gère l'effacement des secteurs concernés
// puis la programmation page par page. La donnée à l'adresse donnée est
// remplacée (le reste du secteur est préservé en RAM).
bool flash_store_write(uint32_t addr, const uint8_t *buf, size_t len);

// Efface tout le circuit (W25Q32 : ~40 s). À utiliser avec précaution.
bool flash_store_erase_all(void);

// ─── Stockage de chaînes (records [len][data...], 0xFF = vierge) ──────────────
#define FLASH_USER_STRING_ADDR   0x000000   // chaîne utilisateur (namespace 0, id 1)
#define FLASH_USER_STRING_MAX    24         // = BIDIB_STRING_MAX

// Lit la chaîne à `addr`. Retourne false si le record est vierge (0xFF),
// invalide, ou si la longueur dépasse buf_size-1.
bool flash_store_read_string(uint32_t addr, char *buf, size_t buf_size);

// Écrit la chaîne `str` (tronquée à FLASH_USER_STRING_MAX) à `addr`.
bool flash_store_write_string(uint32_t addr, const char *str);

#endif /* FLASH_STORE_H_ */