/*
 * HTTP Server Header
 * Simple HTTP server for USB NCM connectivity test
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP server on port 80
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_server_start(void);

/**
 * @brief Stop the HTTP server
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_server_stop(void);

#ifdef __cplusplus
}
#endif
