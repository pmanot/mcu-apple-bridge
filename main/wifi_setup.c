/*
 * WiFi Setup Implementation
 * Connects ESP32-S3 to WiFi network for independent log streaming
 *
 * This provides a debug channel when USB NCM fails on iOS.
 * The same HTTP server (including /logs SSE) is accessible over both
 * USB NCM and WiFi.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

#include "wifi_setup.h"

static const char *TAG = "wifi";

// WiFi credentials
#define WIFI_SSID      "Disarray"
#define WIFI_PASSWORD  "Sp@rkl1ngLapras"
#define WIFI_MAX_RETRY 5

// Event group to signal WiFi connection status
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Current IP address
static char s_ip_str[16] = "not connected";
static int s_retry_count = 0;

/**
 * @brief Convert WiFi auth mode to string
 */
static const char *auth_mode_str(wifi_auth_mode_t authmode)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2_PSK";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3_PSK";
        default:                        return "UNKNOWN";
    }
}

/**
 * @brief Scan for WiFi networks and find our target
 */
static void wifi_scan_and_log(void)
{
    ESP_LOGI(TAG, "Scanning for WiFi networks...");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);  // Blocking scan
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d access points:", ap_count);

    if (ap_count == 0) {
        ESP_LOGW(TAG, "  No networks found!");
        return;
    }

    wifi_ap_record_t *ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        ESP_LOGE(TAG, "  Failed to allocate memory for scan results");
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    bool found_target = false;
    for (int i = 0; i < ap_count; i++) {
        bool is_target = (strcmp((char *)ap_list[i].ssid, WIFI_SSID) == 0);
        if (is_target) found_target = true;

        ESP_LOGI(TAG, "  %s[%d] SSID: %-20s  RSSI: %d  CH: %d  Auth: %s",
                 is_target ? ">>>" : "   ",
                 i + 1,
                 ap_list[i].ssid,
                 ap_list[i].rssi,
                 ap_list[i].primary,
                 auth_mode_str(ap_list[i].authmode));
    }

    if (found_target) {
        ESP_LOGI(TAG, "Target network '%s' FOUND!", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Target network '%s' NOT FOUND in scan!", WIFI_SSID);
    }

    free(ap_list);
}

/**
 * @brief WiFi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                // Do a scan first to verify network is visible
                wifi_scan_and_log();
                ESP_LOGI(TAG, "Connecting to '%s'...", WIFI_SSID);
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;

                // Decode the disconnect reason for debugging
                const char *reason_str = "UNKNOWN";
                switch (event->reason) {
                    case WIFI_REASON_AUTH_EXPIRE:       reason_str = "AUTH_EXPIRE (timeout)"; break;
                    case WIFI_REASON_AUTH_LEAVE:        reason_str = "AUTH_LEAVE"; break;
                    case WIFI_REASON_ASSOC_EXPIRE:      reason_str = "ASSOC_EXPIRE"; break;
                    case WIFI_REASON_ASSOC_LEAVE:       reason_str = "ASSOC_LEAVE"; break;
                    case WIFI_REASON_NO_AP_FOUND:       reason_str = "NO_AP_FOUND"; break;
                    case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_str = "HANDSHAKE_TIMEOUT (wrong password?)"; break;
                    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: reason_str = "4WAY_HANDSHAKE_TIMEOUT (wrong password?)"; break;
                    case WIFI_REASON_CONNECTION_FAIL:   reason_str = "CONNECTION_FAIL"; break;
                    case WIFI_REASON_AUTH_FAIL:         reason_str = "AUTH_FAIL (wrong password?)"; break;
                    default: break;
                }

                ESP_LOGW(TAG, "Disconnected! Reason %d: %s", event->reason, reason_str);

                if (s_retry_count < WIFI_MAX_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retrying (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
                    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second before retry
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "");
                    ESP_LOGE(TAG, "========================================");
                    ESP_LOGE(TAG, "WIFI CONNECTION FAILED");
                    ESP_LOGE(TAG, "========================================");
                    ESP_LOGE(TAG, "  Could not connect after %d attempts", WIFI_MAX_RETRY);
                    ESP_LOGE(TAG, "  SSID: %s", WIFI_SSID);
                    ESP_LOGE(TAG, "  Last error: %s", reason_str);
                    ESP_LOGE(TAG, "");
                    ESP_LOGE(TAG, "  Possible causes:");
                    ESP_LOGE(TAG, "    - Wrong password");
                    ESP_LOGE(TAG, "    - Router MAC filtering");
                    ESP_LOGE(TAG, "    - Router using WPA3-only mode");
                    ESP_LOGE(TAG, "    - Network congestion");
                    ESP_LOGE(TAG, "========================================");
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                strcpy(s_ip_str, "not connected");
                break;
            }

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                ESP_LOGI(TAG, "Connected to '%s' (channel %d)", event->ssid, event->channel);
                ESP_LOGI(TAG, "Waiting for IP address...");
                s_retry_count = 0;
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));

            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "WIFI CONNECTED!");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "  SSID:    %s", WIFI_SSID);
            ESP_LOGI(TAG, "  IP:      %s", s_ip_str);
            ESP_LOGI(TAG, "  Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
            ESP_LOGI(TAG, "  Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "  Access via WiFi:");
            ESP_LOGI(TAG, "    http://%s/", s_ip_str);
            ESP_LOGI(TAG, "    http://%s/logs", s_ip_str);
            ESP_LOGI(TAG, "    http://esp32.local/");
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "");

            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t wifi_init_sta(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WIFI INITIALIZATION");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Target SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "  Password:    %d characters", (int)strlen(WIFI_PASSWORD));
    ESP_LOGI(TAG, "");

    // Create event group
    s_wifi_event_group = xEventGroupCreate();

    // Create default WiFi station netif
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi STA netif");
        return ESP_FAIL;
    }

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Configure WiFi
    wifi_config_t wifi_config = { 0 };

    // Copy credentials
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    // Accept any auth mode (router will tell us what it uses)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    // Enable PMF (Protected Management Frames) - some routers require this
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // WPA3 SAE settings (in case router uses WPA3)
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    // Scan all channels
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start WiFi
    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize mDNS for esp32.local hostname
    ESP_LOGI(TAG, "Initializing mDNS (esp32.local)...");
    ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
    } else {
        mdns_hostname_set("esp32");
        mdns_instance_name_set("ESP32 USB NCM Bridge");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: http://esp32.local/");
    }

    // Don't block - WiFi connects in background
    // The event handler will log success/failure

    return ESP_OK;
}

const char *wifi_get_ip_str(void)
{
    return s_ip_str;
}
