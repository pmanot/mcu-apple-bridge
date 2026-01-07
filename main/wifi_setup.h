/*
 * WiFi Setup Header
 * Connects ESP32-S3 to WiFi network for independent log streaming
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in station mode and connect to AP
 *
 * This runs alongside USB NCM, providing an independent network path
 * for debugging when USB NCM fails on iOS.
 *
 * After connection, the HTTP server (including /logs SSE) is accessible
 * on both:
 *   - USB NCM: http://192.168.7.1/
 *   - WiFi: http://<dhcp-assigned-ip>/
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief Get the WiFi IP address as a string
 * @return IP address string (e.g., "192.168.1.100") or "not connected"
 */
const char *wifi_get_ip_str(void);

#ifdef __cplusplus
}
#endif
