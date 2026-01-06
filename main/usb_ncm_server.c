/*
 * USB NCM Server - Main Application
 *
 * ESP32-S3 USB CDC-NCM Ethernet-over-USB implementation
 * - Exposes USB as network adapter (NCM class)
 * - Runs DHCP server (assigns 192.168.7.x to host)
 * - Serves HTTP content at 192.168.7.1
 *
 * Works with iPhone and macOS without host drivers.
 *
 * BOOT SEQUENCE:
 * ==============
 * 1. NVS init (non-volatile storage for config)
 * 2. TCP/IP stack init (lwIP)
 * 3. Event loop init (for async events)
 * 4. Network init (USB NCM + DHCP server)
 * 5. Log redirect (ESP_LOG -> USB CDC serial)
 * 6. HTTP server start
 *
 * After boot, the device waits for USB host connection.
 * When connected, DHCP assigns an IP and HTTP becomes accessible.
 */

#include <stdio.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "tusb_cdc_acm.h"
#include "tinyusb.h"

#include "network_setup.h"
#include "http_server.h"

static const char *TAG = "main";

/**
 * @brief Custom vprintf for ESP_LOG that outputs to USB CDC-ACM
 *
 * This function intercepts all ESP_LOG output and sends it over
 * the USB CDC-ACM virtual serial port. This allows monitoring
 * logs via `screen /dev/cu.usbmodem* 115200` on macOS.
 *
 * Note: If no terminal is connected (tud_cdc_connected() == false),
 * logs are silently dropped to avoid blocking.
 */
static int cdc_log_vprintf(const char *fmt, va_list args)
{
    char buf[512];  // Increased buffer for longer log lines
    int len = vsnprintf(buf, sizeof(buf), fmt, args);

    if (len > 0) {
        // Truncate if buffer overflow
        if (len >= (int)sizeof(buf)) {
            len = sizeof(buf) - 1;
            buf[len-1] = '\n';  // Ensure newline
        }

        // Only send if USB CDC is connected (terminal open)
        if (tud_cdc_connected()) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t *)buf, len);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
    return len;
}

/**
 * @brief Print system information at startup
 */
static void print_system_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "System Information:");
    ESP_LOGI(TAG, "  Chip:        ESP32-S3");
    ESP_LOGI(TAG, "  Cores:       %d", chip_info.cores);
    ESP_LOGI(TAG, "  Features:    %s%s%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4 " : "");
    ESP_LOGI(TAG, "  Flash:       %s",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "Embedded" : "External");
    ESP_LOGI(TAG, "  Free heap:   %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  IDF version: %s", esp_get_idf_version());
}

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   USB NCM Server for ESP32-S3         ║");
    ESP_LOGI(TAG, "║   Ethernet-over-USB with DHCP         ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");

    print_system_info();

    // =========================================================================
    // STEP 1: Initialize NVS (Non-Volatile Storage)
    // =========================================================================
    // NVS is flash-based key-value storage. Some ESP-IDF components
    // require it (WiFi, BLE, etc.). We init it even though we don't
    // use WiFi, in case any dependency needs it.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 1/6] Initializing NVS flash...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "           NVS partition invalid, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "           NVS initialized successfully");

    // =========================================================================
    // STEP 2: Initialize TCP/IP stack (lwIP)
    // =========================================================================
    // lwIP (lightweight IP) is the TCP/IP stack used by ESP-IDF.
    // This initializes the stack but doesn't create any interfaces yet.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 2/6] Initializing TCP/IP stack (lwIP)...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "           lwIP stack ready");

    // =========================================================================
    // STEP 3: Create default event loop
    // =========================================================================
    // The event loop handles async events (network up/down, IP assigned, etc.)
    // Many ESP-IDF components post events here.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 3/6] Creating event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "           Event loop ready");

    // =========================================================================
    // STEP 4: Initialize USB NCM network
    // =========================================================================
    // This is the main initialization - sets up USB, NCM, esp-netif, DHCP.
    // See network_setup.c for detailed logging of each sub-step.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 4/6] Initializing USB NCM network...");
    ESP_ERROR_CHECK(network_init());

    // =========================================================================
    // STEP 5: Redirect logging to USB CDC
    // =========================================================================
    // After this point, all ESP_LOG output goes to the USB CDC serial port
    // instead of the default UART. Connect a terminal to see logs.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 5/6] Redirecting logs to USB CDC-ACM...");
    esp_log_set_vprintf(cdc_log_vprintf);
    ESP_LOGI(TAG, "           Logs now output to /dev/cu.usbmodem* (macOS)");
    ESP_LOGI(TAG, "           Use: screen /dev/cu.usbmodem* 115200");

    // =========================================================================
    // STEP 6: Start HTTP server
    // =========================================================================
    // The HTTP server listens on port 80 at 192.168.7.1
    // It serves a simple status page and LED control endpoints.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[BOOT 6/6] Starting HTTP server...");
    ESP_ERROR_CHECK(http_server_start());

    // =========================================================================
    // BOOT COMPLETE
    // =========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   BOOT COMPLETE - SERVER READY        ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "How to connect:");
    ESP_LOGI(TAG, "  1. Connect USB-C cable to iPhone/Mac");
    ESP_LOGI(TAG, "  2. Wait for DHCP (automatic)");
    ESP_LOGI(TAG, "  3. Open http://192.168.7.1/ in browser");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Available endpoints:");
    ESP_LOGI(TAG, "  GET  /          - Status page");
    ESP_LOGI(TAG, "  GET  /led       - LED state (JSON)");
    ESP_LOGI(TAG, "  POST /led/on    - Turn LED on");
    ESP_LOGI(TAG, "  POST /led/off   - Turn LED off");
    ESP_LOGI(TAG, "  POST /reset     - Restart ESP32");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Serial monitor:");
    ESP_LOGI(TAG, "  macOS: screen /dev/cu.usbmodem* 115200");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "");
}
