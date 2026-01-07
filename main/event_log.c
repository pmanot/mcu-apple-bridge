/*
 * Event Log Implementation
 * Sticky critical event tracking for USB/NCM/DHCP debugging
 */

#include <string.h>
#include <stdio.h>
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#define MAX_EVENTS 30
#define MAX_DETAIL_LEN 64

// Event names for display
static const char *EVENT_NAMES[] = {
    "USB_MOUNTED",
    "USB_UNMOUNTED",
    "USB_SUSPENDED",
    "USB_RESUMED",
    "NCM_LINK_UP",
    "NETIF_READY",
    "FIRST_RX",
    "FIRST_TX",
    "DHCP_DISCOVER_RX",
    "DHCP_OFFER_TX",
    "DHCP_REQUEST_RX",
    "DHCP_ACK_TX",
    "DHCP_ASSIGNED",
};

typedef struct {
    uint32_t timestamp_ms;
    event_type_t type;
    char detail[MAX_DETAIL_LEN];
} event_entry_t;

static event_entry_t s_events[MAX_EVENTS];
static int s_event_count = 0;
static bool s_event_occurred[EVT_COUNT] = {false};
static SemaphoreHandle_t s_mutex = NULL;

void event_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_event_count = 0;
    memset(s_events, 0, sizeof(s_events));
    memset(s_event_occurred, 0, sizeof(s_event_occurred));
}

void event_log_record(event_type_t type, const char *detail)
{
    if (!s_mutex || type >= EVT_COUNT) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Mark event as occurred
        s_event_occurred[type] = true;

        // Add to event list if room
        if (s_event_count < MAX_EVENTS) {
            event_entry_t *e = &s_events[s_event_count];
            e->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            e->type = type;

            if (detail) {
                strncpy(e->detail, detail, MAX_DETAIL_LEN - 1);
                e->detail[MAX_DETAIL_LEN - 1] = '\0';
            } else {
                e->detail[0] = '\0';
            }

            s_event_count++;
        }

        xSemaphoreGive(s_mutex);
    }
}

bool event_log_has(event_type_t type)
{
    if (!s_mutex || type >= EVT_COUNT) return false;

    bool result = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        result = s_event_occurred[type];
        xSemaphoreGive(s_mutex);
    }
    return result;
}

size_t event_log_get_all(char *buf, size_t size)
{
    if (!s_mutex || !buf || size == 0) return 0;

    size_t written = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Header
        written += snprintf(buf + written, size - written,
            "=== CRITICAL EVENTS (%d recorded) ===\n\n", s_event_count);

        // Each event
        for (int i = 0; i < s_event_count && written < size - 100; i++) {
            event_entry_t *e = &s_events[i];
            const char *name = (e->type < EVT_COUNT) ? EVENT_NAMES[e->type] : "UNKNOWN";

            if (e->detail[0]) {
                written += snprintf(buf + written, size - written,
                    "[%6lu ms] %s: %s\n",
                    (unsigned long)e->timestamp_ms, name, e->detail);
            } else {
                written += snprintf(buf + written, size - written,
                    "[%6lu ms] %s\n",
                    (unsigned long)e->timestamp_ms, name);
            }
        }

        // Summary of what happened/didn't happen
        written += snprintf(buf + written, size - written, "\n=== STATUS FLAGS ===\n");
        for (int i = 0; i < EVT_COUNT && written < size - 50; i++) {
            written += snprintf(buf + written, size - written,
                "%s: %s\n",
                EVENT_NAMES[i],
                s_event_occurred[i] ? "YES" : "NO");
        }

        xSemaphoreGive(s_mutex);
    }

    return written;
}

size_t event_log_get_status_json(char *buf, size_t size)
{
    if (!s_mutex || !buf || size == 0) return 0;

    size_t written = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        written += snprintf(buf + written, size - written, "{\n");

        for (int i = 0; i < EVT_COUNT && written < size - 50; i++) {
            written += snprintf(buf + written, size - written,
                "  \"%s\": %s%s\n",
                EVENT_NAMES[i],
                s_event_occurred[i] ? "true" : "false",
                (i < EVT_COUNT - 1) ? "," : "");
        }

        written += snprintf(buf + written, size - written, "}\n");

        xSemaphoreGive(s_mutex);
    }

    return written;
}
