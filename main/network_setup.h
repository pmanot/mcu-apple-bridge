/*
 * USB NCM Network Setup Header
 * Configures USB CDC-NCM device with DHCP server for iPhone/macOS connectivity
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB NCM network interface with DHCP server
 *
 * This function:
 * - Initializes TinyUSB driver
 * - Creates USB NCM device
 * - Sets up esp-netif with static IP (192.168.7.1)
 * - Starts DHCP server (pool: 192.168.7.2-10)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t network_init(void);

#ifdef __cplusplus
}
#endif
