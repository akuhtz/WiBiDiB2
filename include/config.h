/*
 * config.h  —  Pico 2W WiThrottle/BiDiB gateway
 * Portage depuis ESP32
 * Pierre Moulin
 */

#ifndef CONFIG_H_
#define CONFIG_H_

// ─── WiFi STA ─────────────────────────────────────────────────────────────────
// Le Pico rejoint un réseau WiFi existant (mode par défaut).
// Un fichier local include/network_config.h (HORS contrôle de version) peut
// surcharger WIFI_SSID / WIFI_PASSWORD. Copier network_config.example.h.
#if __has_include("network_config.h")
#include "network_config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID            "myssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD        "mypassword"
#endif
// Délai (ms) pour la connexion STA avant bascule en mode AP
#define WIFI_STA_TIMEOUT_MS  20000

// ─── WiFi AP ──────────────────────────────────────────────────────────────────
// Fallback : le Pico démarre son propre réseau avec ce SSID/mot de passe
#define WIFI_AP_SSID         "myssid"
#define WIFI_AP_PASSWORD     "mypassword"
// Adresse IP fixe du Pico en mode AP
#define AP_IP_ADDR           "192.168.4.1"

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

// ─── Distributed control ────────────────────────────────────────────────────────
#define BIDIB_DISTRIBUTED_CONTROL  1

#endif /* CONFIG_H_ */
