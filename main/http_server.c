/*
 * HTTP Server Implementation
 * Simple HTTP server for USB NCM connectivity test
 */

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "http_server.h"

#define LED_GPIO 21  // Built-in LED (same as LED_BUILTIN in Arduino)
#define LED_ON  0    // Active-low: drive LOW to turn on
#define LED_OFF 1    // Active-low: drive HIGH to turn off
static bool s_led_state = false;

static const char *TAG = "http_server";

static httpd_handle_t s_server = NULL;

// Simple HTML response
static const char *HTML_RESPONSE =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP32-S3 USB NCM</title>"
    "<style>"
    "body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; "
    "max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }"
    "h1 { color: #333; }"
    ".success { color: #28a745; font-size: 24px; }"
    ".info { background: #f8f9fa; padding: 15px; border-radius: 8px; margin: 20px 0; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>ESP32-S3 USB NCM Server</h1>"
    "<p class=\"success\">Connected!</p>"
    "<div class=\"info\">"
    "<p>USB Ethernet-over-USB (CDC-NCM) is working.</p>"
    "<p>Server IP: 192.168.7.1</p>"
    "</div>"
    "</body>"
    "</html>";

/**
 * @brief Handler for GET /
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving root page to %s", req->uri);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_RESPONSE, strlen(HTML_RESPONSE));

    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for POST /reset - triggers ESP32 software reset
 * Call this from your iOS app if connection becomes stale after replug
 */
static esp_err_t reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reset requested - restarting ESP32");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"resetting\"}");

    // Small delay to let response send before reset
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();

    return ESP_OK;  // Never reached
}

static const httpd_uri_t reset_uri = {
    .uri       = "/reset",
    .method    = HTTP_POST,
    .handler   = reset_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for POST /led/on - turn LED on
 */
static esp_err_t led_on_handler(httpd_req_t *req)
{
    s_led_state = true;
    gpio_set_level(LED_GPIO, LED_ON);
    ESP_LOGI(TAG, "LED ON");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"led\":true}");
    return ESP_OK;
}

/**
 * @brief Handler for POST /led/off - turn LED off
 */
static esp_err_t led_off_handler(httpd_req_t *req)
{
    s_led_state = false;
    gpio_set_level(LED_GPIO, LED_OFF);
    ESP_LOGI(TAG, "LED OFF");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"led\":false}");
    return ESP_OK;
}

/**
 * @brief Handler for GET /led - get LED state
 */
static esp_err_t led_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    if (s_led_state) {
        httpd_resp_sendstr(req, "{\"led\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"led\":false}");
    }
    return ESP_OK;
}

static const httpd_uri_t led_on_uri = {
    .uri       = "/led/on",
    .method    = HTTP_POST,
    .handler   = led_on_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t led_off_uri = {
    .uri       = "/led/off",
    .method    = HTTP_POST,
    .handler   = led_off_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t led_status_uri = {
    .uri       = "/led",
    .method    = HTTP_GET,
    .handler   = led_status_handler,
    .user_ctx  = NULL
};

esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    // Initialize LED GPIO
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_OFF);  // Start with LED off (HIGH for active-low)
    ESP_LOGI(TAG, "LED GPIO %d initialized", LED_GPIO);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &reset_uri);
    httpd_register_uri_handler(s_server, &led_on_uri);
    httpd_register_uri_handler(s_server, &led_off_uri);
    httpd_register_uri_handler(s_server, &led_status_uri);

    ESP_LOGI(TAG, "HTTP server started at http://192.168.7.1/");

    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;

    return ret;
}
