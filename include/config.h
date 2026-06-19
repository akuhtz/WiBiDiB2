/*
 * config.h  —  Pico 2W WiThrottle/BiDiB gateway
 * Portage depuis ESP32
 * Pierre Moulin
 */

#ifndef CONFIG_H_
#define CONFIG_H_

// ─── WiFi AP ────────────────────────────────────────────────────────────────
#define WIFI_SSID        "myssid"
#define WIFI_PASSWORD    "mypassword"
// Adresse IP fixe du Pico en mode AP
#define AP_IP_ADDR       "192.168.4.1"

// ─── TCP serveur WiThrottle ──────────────────────────────────────────────────
#define WITHROTTLE_PORT  5550
#define MAX_CLIENTS      4        // Engine Driver supporte jusqu'à 4 throttles simultanés

// ─── Throttles / locos ───────────────────────────────────────────────────────
#define MAX_THROTTLES    4        // nombre de connexions TCP simultanées

// ─── Heartbeat WiThrottle ────────────────────────────────────────────────────
#define HEARTBEAT_TIMEOUT_S  10   // secondes avant emergency stop

// ─── Timers (pico-sdk) ───────────────────────────────────────────────────────
// time_us_64()  → µs  (remplace esp_timer_get_time())
// to_ms_since_boot(get_absolute_time()) → ms (remplace millis())
#define now_ms()  ((uint32_t)(time_us_64() / 1000ULL))

// ─── Debug ───────────────────────────────────────────────────────────────────
#define LOG_INFO(tag, fmt, ...)   printf("[I] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)   printf("[W] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...)  printf("[E] %s: " fmt "\n", tag, ##__VA_ARGS__)
#define debug      1    // 1 pour activer les logs de debug (très verbeux)

#endif /* CONFIG_H_ */
