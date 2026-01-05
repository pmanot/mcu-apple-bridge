/*
 * USB NCM Server - Main Application
 *
 * ESP32-S3 USB CDC-NCM Ethernet-over-USB implementation
 * - Exposes USB as network adapter (NCM class)
 * - Runs DHCP server (assigns 192.168.7.x to host)
 * - Serves HTTP content at 192.168.7.1
 *
 * Works with iPhone and macOS without host drivers.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "network_setup.h"
#include "http_server.h"

static const char *TAG = "usb_ncm_server";

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  USB NCM Server for ESP32-S3");
    ESP_LOGI(TAG, "=================================");

    // Initialize NVS (required for some components)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize USB NCM network interface with DHCP server
    ESP_ERROR_CHECK(network_init());

    // Start HTTP server
    ESP_ERROR_CHECK(http_server_start());

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  Server ready!");
    ESP_LOGI(TAG, "  Connect via USB-C and open:");
    ESP_LOGI(TAG, "  http://192.168.7.1/");
    ESP_LOGI(TAG, "=================================");
}
