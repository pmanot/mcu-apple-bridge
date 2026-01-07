/*
 * HTTP Server Implementation
 * Simple HTTP server for USB NCM connectivity test
 *
 * This server runs on lwIP's TCP stack at 192.168.7.1:80
 * It provides:
 *   - Status page (GET /)
 *   - LED control (GET/POST /led, /led/on, /led/off)
 *   - Device reset (POST /reset)
 *
 * The esp_http_server component handles:
 *   - TCP connection management
 *   - HTTP parsing (headers, body)
 *   - URI routing
 *   - Response generation
 */

#include <string.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "http_server.h"
#include "log_stream.h"
#include "event_log.h"

#define LED_GPIO 21  // Built-in LED (same as LED_BUILTIN in Arduino)
#define LED_ON  0    // Active-low: drive LOW to turn on
#define LED_OFF 1    // Active-low: drive HIGH to turn off
static bool s_led_state = false;

static const char *TAG = "http";

static httpd_handle_t s_server = NULL;

// Request counter for logging
static uint32_t s_request_count = 0;

// Simple HTML response with inline styles
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
 * @brief Log HTTP request details
 */
static void log_request(httpd_req_t *req, const char *handler_name)
{
    s_request_count++;

    // Get client info if available
    int sockfd = httpd_req_to_sockfd(req);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "+-- HTTP REQUEST #%lu --------------------",
             (unsigned long)s_request_count);
    ESP_LOGI(TAG, "| Method:  %s", http_method_str(req->method));
    ESP_LOGI(TAG, "| URI:     %s", req->uri);
    ESP_LOGI(TAG, "| Handler: %s", handler_name);
    ESP_LOGI(TAG, "| Socket:  %d", sockfd);
    ESP_LOGI(TAG, "| Content: %d bytes", req->content_len);
}

/**
 * @brief Log HTTP response
 */
static void log_response(int status_code, const char *content_type, size_t body_len)
{
    ESP_LOGI(TAG, "|");
    ESP_LOGI(TAG, "| Response: %d", status_code);
    ESP_LOGI(TAG, "| Type:     %s", content_type);
    ESP_LOGI(TAG, "| Size:     %zu bytes", body_len);
    ESP_LOGI(TAG, "+----------------------------------------");
    ESP_LOGI(TAG, "");
}

/**
 * @brief Handler for GET /
 *
 * Serves the main status page. This is the first thing users see
 * when they connect and open http://192.168.7.1/ in their browser.
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    log_request(req, "root_handler");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_RESPONSE, strlen(HTML_RESPONSE));

    log_response(200, "text/html", strlen(HTML_RESPONSE));

    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for POST /reset
 *
 * Triggers a software reset of the ESP32. Useful if the USB
 * connection becomes stale after unplug/replug cycles.
 */
static esp_err_t reset_handler(httpd_req_t *req)
{
    log_request(req, "reset_handler");

    ESP_LOGW(TAG, "|");
    ESP_LOGW(TAG, "| !!! RESET REQUESTED !!!");
    ESP_LOGW(TAG, "| Device will restart in 100ms");

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"resetting\"}";
    httpd_resp_sendstr(req, response);

    log_response(200, "application/json", strlen(response));

    // Small delay to let response send before reset
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Calling esp_restart()...");
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
 * @brief Handler for POST /led/on
 *
 * Turns the LED on (GPIO 21, active-low).
 */
static esp_err_t led_on_handler(httpd_req_t *req)
{
    log_request(req, "led_on_handler");

    s_led_state = true;
    gpio_set_level(LED_GPIO, LED_ON);

    ESP_LOGI(TAG, "|");
    ESP_LOGI(TAG, "| LED STATE: ON (GPIO %d = LOW)", LED_GPIO);

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"led\":true}";
    httpd_resp_sendstr(req, response);

    log_response(200, "application/json", strlen(response));

    return ESP_OK;
}

/**
 * @brief Handler for POST /led/off
 *
 * Turns the LED off (GPIO 21, active-low).
 */
static esp_err_t led_off_handler(httpd_req_t *req)
{
    log_request(req, "led_off_handler");

    s_led_state = false;
    gpio_set_level(LED_GPIO, LED_OFF);

    ESP_LOGI(TAG, "|");
    ESP_LOGI(TAG, "| LED STATE: OFF (GPIO %d = HIGH)", LED_GPIO);

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"led\":false}";
    httpd_resp_sendstr(req, response);

    log_response(200, "application/json", strlen(response));

    return ESP_OK;
}

/**
 * @brief Handler for GET /led
 *
 * Returns current LED state as JSON.
 */
static esp_err_t led_status_handler(httpd_req_t *req)
{
    log_request(req, "led_status_handler");

    ESP_LOGI(TAG, "|");
    ESP_LOGI(TAG, "| Current LED state: %s", s_led_state ? "ON" : "OFF");

    httpd_resp_set_type(req, "application/json");
    const char *response = s_led_state ? "{\"led\":true}" : "{\"led\":false}";
    httpd_resp_sendstr(req, response);

    log_response(200, "application/json", strlen(response));

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

/**
 * @brief Handler for GET /logs - Server-Sent Events log stream
 *
 * Streams logs in real-time using SSE format:
 *   data: log line here\n\n
 *
 * iOS Usage with URLSession:
 *   let url = URL(string: "http://192.168.7.1/logs")!
 *   let task = URLSession.shared.dataTask(with: url) { data, _, _ in
 *       if let data = data, let line = String(data: data, encoding: .utf8) {
 *           // Parse SSE: lines starting with "data: "
 *       }
 *   }
 *   task.resume()
 *
 * Or use a proper EventSource library for Swift.
 */
static esp_err_t logs_sse_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "+-- SSE LOG STREAM STARTED -------------");
    ESP_LOGI(TAG, "| Client connected for log streaming");

    // Allocate a reader slot
    int reader_id = log_buffer_alloc_reader();
    if (reader_id < 0) {
        ESP_LOGW(TAG, "| No reader slots available (max 4 clients)");
        ESP_LOGI(TAG, "+----------------------------------------");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Too many log clients (max 4)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "| Allocated reader ID: %d", reader_id);
    ESP_LOGI(TAG, "+----------------------------------------");

    // Set SSE headers
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Send initial comment to establish connection
    const char *init_msg = ": ESP32 log stream connected\n\n";
    if (httpd_resp_send_chunk(req, init_msg, strlen(init_msg)) != ESP_OK) {
        log_buffer_free_reader(reader_id);
        return ESP_FAIL;
    }

    // Stream logs until client disconnects
    char sse_buf[300];  // "data: " + log line + "\n\n"
    int idle_count = 0;
    const int MAX_IDLE = 100;  // Send keepalive after this many empty polls

    while (1) {
        size_t log_len;
        const char *log_line = log_buffer_read(reader_id, &log_len);

        if (log_line && log_len > 0) {
            idle_count = 0;

            // Format as SSE: "data: <log>\n\n"
            int sse_len = snprintf(sse_buf, sizeof(sse_buf), "data: %.*s\n\n",
                                   (int)log_len, log_line);

            if (httpd_resp_send_chunk(req, sse_buf, sse_len) != ESP_OK) {
                // Client disconnected
                break;
            }
        } else {
            idle_count++;

            // Send keepalive comment periodically to detect disconnect
            if (idle_count >= MAX_IDLE) {
                idle_count = 0;
                if (httpd_resp_send_chunk(req, ": keepalive\n\n", 13) != ESP_OK) {
                    break;
                }
            }

            // Small delay when no data
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    // Cleanup
    log_buffer_free_reader(reader_id);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "+-- SSE LOG STREAM ENDED ---------------");
    ESP_LOGI(TAG, "| Reader %d disconnected", reader_id);
    ESP_LOGI(TAG, "+----------------------------------------");

    // End chunked response
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static const httpd_uri_t logs_sse_uri = {
    .uri       = "/logs",
    .method    = HTTP_GET,
    .handler   = logs_sse_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for GET /logs_all - Static log dump
 *
 * Returns all buffered logs as plain text (not SSE).
 * Useful for viewing logs from before connecting to the stream.
 */
static esp_err_t logs_all_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "+-- LOGS_ALL REQUEST -------------------");

    int line_count = log_buffer_get_count();
    ESP_LOGI(TAG, "| Returning %d buffered log lines", line_count);

    // Allocate buffer for all logs (100 lines * 256 bytes max = 25KB)
    #define LOG_DUMP_SIZE 32768
    char *chunk = malloc(LOG_DUMP_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "| malloc failed for log buffer");
        ESP_LOGI(TAG, "+----------------------------------------");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    // Get all logs
    size_t total_len = log_buffer_get_all(chunk, LOG_DUMP_SIZE);

    if (total_len > 0) {
        httpd_resp_send(req, chunk, total_len);
    } else {
        const char *empty_msg = "(no logs in buffer)\n";
        httpd_resp_send(req, empty_msg, strlen(empty_msg));
    }

    ESP_LOGI(TAG, "| Sent %zu bytes", total_len);
    ESP_LOGI(TAG, "+----------------------------------------");

    free(chunk);
    return ESP_OK;
}

static const httpd_uri_t logs_all_uri = {
    .uri       = "/logs_all",
    .method    = HTTP_GET,
    .handler   = logs_all_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for GET /events - Critical events (sticky, never truncated)
 */
static esp_err_t events_handler(httpd_req_t *req)
{
    #define EVENTS_BUF_SIZE 4096
    char *buf = malloc(EVENTS_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    size_t len = event_log_get_all(buf, EVENTS_BUF_SIZE);
    httpd_resp_send(req, buf, len);

    free(buf);
    return ESP_OK;
}

static const httpd_uri_t events_uri = {
    .uri       = "/events",
    .method    = HTTP_GET,
    .handler   = events_handler,
    .user_ctx  = NULL
};

/**
 * @brief Handler for GET /status - JSON with event flags
 */
static esp_err_t status_handler(httpd_req_t *req)
{
    #define STATUS_BUF_SIZE 1024
    char *buf = malloc(STATUS_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    size_t len = event_log_get_status_json(buf, STATUS_BUF_SIZE);
    httpd_resp_send(req, buf, len);

    free(buf);
    return ESP_OK;
}

static const httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
};

/**
 * @brief Start the HTTP server
 *
 * Creates the HTTP server on port 80 and registers all URI handlers.
 * The server uses esp_http_server which is built on lwIP's TCP stack.
 */
esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running, skipping init");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Starting HTTP server...");

    // Initialize LED GPIO
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_OFF);  // Start with LED off
    ESP_LOGI(TAG, "  LED GPIO %d initialized (active-low)", LED_GPIO);

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;  // Close stale connections
    config.server_port = 80;
    config.max_uri_handlers = 12;    // We have 9 handlers, leave room for more

    ESP_LOGI(TAG, "  Port: %d", config.server_port);
    ESP_LOGI(TAG, "  Max URI handlers: %d", config.max_uri_handlers);
    ESP_LOGI(TAG, "  Max connections: %d", config.max_open_sockets);
    ESP_LOGI(TAG, "  LRU purge: enabled");

    // Start the server
    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "  FAILED to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Registering URI handlers:");
    ESP_LOGI(TAG, "  GET  /          -> root_handler (status page)");
    httpd_register_uri_handler(s_server, &root_uri);

    ESP_LOGI(TAG, "  GET  /led       -> led_status_handler (get state)");
    httpd_register_uri_handler(s_server, &led_status_uri);

    ESP_LOGI(TAG, "  POST /led/on    -> led_on_handler");
    httpd_register_uri_handler(s_server, &led_on_uri);

    ESP_LOGI(TAG, "  POST /led/off   -> led_off_handler");
    httpd_register_uri_handler(s_server, &led_off_uri);

    ESP_LOGI(TAG, "  POST /reset     -> reset_handler (restart device)");
    httpd_register_uri_handler(s_server, &reset_uri);

    ESP_LOGI(TAG, "  GET  /logs      -> logs_sse_handler (SSE log stream)");
    httpd_register_uri_handler(s_server, &logs_sse_uri);

    ESP_LOGI(TAG, "  GET  /logs_all  -> logs_all_handler (all buffered logs)");
    httpd_register_uri_handler(s_server, &logs_all_uri);

    ESP_LOGI(TAG, "  GET  /events    -> events_handler (critical events)");
    httpd_register_uri_handler(s_server, &events_uri);

    ESP_LOGI(TAG, "  GET  /status    -> status_handler (event flags JSON)");
    httpd_register_uri_handler(s_server, &status_uri);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "HTTP server started at http://192.168.7.1/");
    ESP_LOGI(TAG, "");

    return ESP_OK;
}

/**
 * @brief Stop the HTTP server
 */
esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping HTTP server...");
    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "HTTP server stopped");
    } else {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
    }

    return ret;
}
