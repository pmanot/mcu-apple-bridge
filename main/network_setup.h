/*
 * USB NCM Network Setup Header
 * Configures USB CDC-NCM device with DHCP server for iPhone/macOS connectivity
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB NCM network interface with DHCP server
 *
 * This function:
 * - Initializes TinyUSB driver
 * - Initializes CDC-ACM for serial logging
 * - Creates USB NCM device
 * - Sets up esp-netif with static IP (192.168.7.1)
 * - Starts DHCP server (pool: 192.168.7.2-10)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t network_init(void);

/**
 * @brief Get network statistics
 *
 * @param rx_pkts     Output: number of received packets (can be NULL)
 * @param tx_pkts     Output: number of transmitted packets (can be NULL)
 * @param rx_bytes    Output: total bytes received (can be NULL)
 * @param tx_bytes    Output: total bytes transmitted (can be NULL)
 */
void network_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts,
                       uint32_t *rx_bytes, uint32_t *tx_bytes);

#ifdef __cplusplus
}
#endif
