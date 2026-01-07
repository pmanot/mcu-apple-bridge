/*
 * Event Log - Sticky critical event tracking
 *
 * Unlike the rolling log buffer, these events are NEVER overwritten.
 * Used to track critical USB/NCM/DHCP events for debugging.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVT_USB_MOUNTED,        // TinyUSB device configured by host
    EVT_USB_UNMOUNTED,      // TinyUSB device disconnected
    EVT_USB_SUSPENDED,      // USB suspended
    EVT_USB_RESUMED,        // USB resumed
    EVT_NCM_LINK_UP,        // on_usb_net_init() called - NCM ready
    EVT_NETIF_READY,        // esp-netif started
    EVT_FIRST_RX,           // First packet received from host
    EVT_FIRST_TX,           // First packet sent to host
    EVT_DHCP_DISCOVER_RX,   // DHCP DISCOVER received from host
    EVT_DHCP_OFFER_TX,      // DHCP OFFER sent to host
    EVT_DHCP_REQUEST_RX,    // DHCP REQUEST received from host
    EVT_DHCP_ACK_TX,        // DHCP ACK sent to host
    EVT_DHCP_ASSIGNED,      // DHCP server assigned IP
    EVT_COUNT               // Number of event types
} event_type_t;

/**
 * @brief Initialize the event log
 * Call once at startup before any events are recorded.
 */
void event_log_init(void);

/**
 * @brief Record a critical event
 * Events are stored permanently until reboot (never overwritten).
 *
 * @param type Event type
 * @param detail Optional detail string (can be NULL)
 */
void event_log_record(event_type_t type, const char *detail);

/**
 * @brief Check if a specific event type has occurred
 * @param type Event type to check
 * @return true if event was recorded at least once
 */
bool event_log_has(event_type_t type);

/**
 * @brief Get all events as formatted text
 *
 * @param buf Output buffer
 * @param size Buffer size
 * @return Number of bytes written
 */
size_t event_log_get_all(char *buf, size_t size);

/**
 * @brief Get status JSON with boolean flags for each event type
 *
 * @param buf Output buffer
 * @param size Buffer size
 * @return Number of bytes written
 */
size_t event_log_get_status_json(char *buf, size_t size);

#ifdef __cplusplus
}
#endif
