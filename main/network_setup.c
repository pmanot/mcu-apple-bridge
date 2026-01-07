/*
 * USB NCM Network Setup (self-healing for iOS)
 *
 * Fixes:
 *  - Drive NCM link state explicitly (DOWN until stack ready; UP to trigger DHCP)
 *  - Recover from "early connect" by forcing USB detach/attach if no RX after mount
 *  - Initialize tinyusb_net early to avoid enumeration races
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb_cdc_acm.h"

// TinyUSB core (tud_* APIs)
#include "tusb.h"

#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"

#include "network_setup.h"
#include "event_log.h"

static const char *TAG = "net";

// ----------------------------
// Configuration knobs
// ----------------------------
#define USB_LINK_KICK_DELAY_MS        250     // delay between DOWN->UP (iOS notices)
#define USB_NO_RX_GRACE_MS            2000    // after mount, wait this long for any RX
#define USB_RECOVER_DETACH_MS         400     // how long to stay "detached"
#define USB_RECOVER_POST_ATTACH_MS    400     // settle time after attach
#define USB_RECOVER_LOOP_PERIOD_MS    250     // watchdog loop tick
#define USB_RECOVER_MAX_ATTEMPTS      5       // per mount cycle
#define USB_RECOVER_BACKOFF_START_MS  2500
#define USB_RECOVER_BACKOFF_MAX_MS    15000

// ----------------------------
// State
// ----------------------------
static esp_netif_t *s_netif = NULL;

static uint32_t s_rx_packets = 0;
static uint32_t s_tx_packets = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_tx_bytes = 0;

static bool s_first_rx_logged = false;
static bool s_first_tx_logged = false;

static volatile bool s_stack_ready = false;     // lwIP/netif + DHCP up
static volatile bool s_usb_mounted = false;     // USB configured by host
static volatile bool s_link_up = false;         // our driven NCM link state

static volatile uint32_t s_mount_ms = 0;
static volatile uint32_t s_last_rx_ms = 0;

static uint32_t s_last_recover_ms = 0;
static uint32_t s_backoff_ms = USB_RECOVER_BACKOFF_START_MS;
static uint32_t s_recover_attempts = 0;

static TaskHandle_t s_usb_watchdog_task = NULL;

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ----------------------------
// IP config
// ----------------------------
static const esp_netif_ip_info_t s_usb_ip_info = {
    .ip      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
    .gw      = { .addr = ESP_IP4TOADDR(192, 168, 7, 254) },  // fake gw
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

// ----------------------------
// Helpers
// ----------------------------
static void usb_set_link_state(bool up, const char *reason)
{
    if (s_link_up == up) {
        // still log transitions only
    } else {
        s_link_up = up;
    }

    // This is the notification iOS keys off for DHCP.
    tud_network_link_state(0, up);

    if (up) {
        // Record "NCM_LINK_UP" even though TinyUSB NCM never calls tud_network_init_cb()
        event_log_record(EVT_NCM_LINK_UP, reason);
        ESP_LOGW(TAG, "*** USB NCM LINK UP *** (%s)", reason ? reason : "no_reason");
    } else {
        ESP_LOGW(TAG, "*** USB NCM LINK DOWN *** (%s)", reason ? reason : "no_reason");
    }
}

static void l2_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

// ----------------------------
// TinyUSB device callbacks
// ----------------------------
void tud_mount_cb(void)
{
    s_usb_mounted = true;
    s_mount_ms = now_ms();
    s_last_rx_ms = 0;

    s_recover_attempts = 0;
    s_backoff_ms = USB_RECOVER_BACKOFF_START_MS;
    s_last_recover_ms = 0;

    event_log_record(EVT_USB_MOUNTED, NULL);
    ESP_LOGW(TAG, "*** USB MOUNTED (device configured by host) ***");

    // Always start DOWN. We'll bring it UP only once stack_ready is true.
    usb_set_link_state(false, "mounted");
}

void tud_umount_cb(void)
{
    s_usb_mounted = false;
    s_mount_ms = 0;
    s_last_rx_ms = 0;

    // Reset per-mount events
    s_first_rx_logged = false;
    s_first_tx_logged = false;

    event_log_record(EVT_USB_UNMOUNTED, NULL);
    ESP_LOGW(TAG, "*** USB UNMOUNTED ***");

    usb_set_link_state(false, "unmounted");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    event_log_record(EVT_USB_SUSPENDED, remote_wakeup_en ? "wake_en" : NULL);
    ESP_LOGW(TAG, "*** USB SUSPENDED (remote_wakeup=%d) ***", remote_wakeup_en);

    // Keep link DOWN during suspend to encourage sane retry on resume.
    usb_set_link_state(false, "suspended");
}

void tud_resume_cb(void)
{
    event_log_record(EVT_USB_RESUMED, NULL);
    ESP_LOGW(TAG, "*** USB RESUMED ***");

    // If we're mounted and stack is ready, re-kick link UP to force DHCP reacquire.
    if (s_usb_mounted && s_stack_ready) {
        usb_set_link_state(false, "resume_kick_down");
        vTaskDelay(pdMS_TO_TICKS(USB_LINK_KICK_DELAY_MS));
        usb_set_link_state(true, "resume_kick_up");
    }
}

// ----------------------------
// NCM callbacks
// ----------------------------
static void on_usb_net_init(void *ctx)
{
    // NOTE: TinyUSB NCM often never calls this. Keep for completeness.
    (void)ctx;
    ESP_LOGW(TAG, "*** on_usb_net_init() called (rare on NCM) ***");
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;

    if (!s_netif) {
        ESP_LOGW(TAG, "RX: netif not ready, dropping");
        return ESP_OK;
    }

    s_rx_packets++;
    s_rx_bytes += len;

    uint32_t t = now_ms();
    s_last_rx_ms = t;

    if (!s_first_rx_logged) {
        s_first_rx_logged = true;
        event_log_record(EVT_FIRST_RX, NULL);
    }

    // quick DHCP detect for your event log
    if (len >= 42) {
        uint8_t *eth = (uint8_t *)buffer;
        uint16_t ethertype = (uint16_t)((eth[12] << 8) | eth[13]);
        if (ethertype == 0x0800) {
            uint8_t proto = eth[23];
            if (proto == 17) {
                uint16_t src_port = (uint16_t)((eth[34] << 8) | eth[35]);
                uint16_t dst_port = (uint16_t)((eth[36] << 8) | eth[37]);
                if (src_port == 68 && dst_port == 67) {
                    event_log_record(EVT_DHCP_DISCOVER_RX, NULL);
                }
            }
        }
    }

    // Must copy - TinyUSB reuses RX buffer
    void *buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "RX: malloc(%u) failed", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);

    esp_err_t ret = esp_netif_receive(s_netif, buf_copy, len, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX: esp_netif_receive failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;

    // Don't try to TX if we're not in a sane state.
    if (!s_usb_mounted || !s_link_up) {
        return ESP_OK;
    }

    s_tx_packets++;
    s_tx_bytes += len;

    if (!s_first_tx_logged) {
        s_first_tx_logged = true;
        event_log_record(EVT_FIRST_TX, NULL);
    }

    // crude DHCP response detection (for event log)
    if (len >= 42) {
        uint8_t *eth = (uint8_t *)buffer;
        uint16_t ethertype = (uint16_t)((eth[12] << 8) | eth[13]);
        if (ethertype == 0x0800) {
            uint8_t proto = eth[23];
            if (proto == 17) {
                uint16_t src_port = (uint16_t)((eth[34] << 8) | eth[35]);
                uint16_t dst_port = (uint16_t)((eth[36] << 8) | eth[37]);
                if (src_port == 67 && dst_port == 68) {
                    event_log_record(EVT_DHCP_OFFER_TX, NULL); // OFFER/ACK both look like this here
                }
            }
        }
    }

    // Retry a couple times; iOS DHCP bursts are tight.
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(250));
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX FAILED: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

// ----------------------------
// USB watchdog task
// ----------------------------
static void usb_watchdog_task(void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "*** USB WATCHDOG TASK STARTED ***");

    while (1) {
        uint32_t t = now_ms();

        // If stack becomes ready while already mounted, kick link UP once.
        if (s_stack_ready && s_usb_mounted && !s_link_up) {
            usb_set_link_state(false, "stack_ready_kick_down");
            vTaskDelay(pdMS_TO_TICKS(USB_LINK_KICK_DELAY_MS));
            usb_set_link_state(true, "stack_ready_kick_up");
        }

        // If mounted + stack ready + link up, but zero RX after grace => force a real USB reattach.
        if (s_stack_ready && s_usb_mounted) {
            bool has_rx = (s_last_rx_ms != 0);

            if (!has_rx && s_mount_ms != 0) {
                uint32_t since_mount = t - s_mount_ms;
                uint32_t since_recover = (s_last_recover_ms == 0) ? 0 : (t - s_last_recover_ms);

                if (since_mount >= USB_NO_RX_GRACE_MS &&
                    s_recover_attempts < USB_RECOVER_MAX_ATTEMPTS &&
                    (s_last_recover_ms == 0 || since_recover >= s_backoff_ms)) {

                    s_recover_attempts++;
                    s_last_recover_ms = t;

                    usb_set_link_state(false, "no_rx_after_mount");

                    ESP_LOGW(TAG, "*** USB RECOVER: tud_disconnect/tud_connect (attempt %lu) ***",
                             (unsigned long)s_recover_attempts);

                    // Force host to re-enumerate; this is what actually clears iOS's "gave up" state.
                    tud_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(USB_RECOVER_DETACH_MS));
                    tud_connect();

                    // Reset per-mount timing so the grace window restarts post-reattach.
                    s_mount_ms = now_ms();
                    s_last_rx_ms = 0;

                    vTaskDelay(pdMS_TO_TICKS(USB_RECOVER_POST_ATTACH_MS));

                    // Kick link UP again to trigger DHCP.
                    usb_set_link_state(false, "post_attach_kick_down");
                    vTaskDelay(pdMS_TO_TICKS(USB_LINK_KICK_DELAY_MS));
                    usb_set_link_state(true, "kick_complete");

                    // Exponential backoff (avoid thrashing)
                    if (s_backoff_ms < USB_RECOVER_BACKOFF_MAX_MS) {
                        uint32_t next = s_backoff_ms * 2;
                        s_backoff_ms = (next > USB_RECOVER_BACKOFF_MAX_MS) ? USB_RECOVER_BACKOFF_MAX_MS : next;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(USB_RECOVER_LOOP_PERIOD_MS));
    }
}

// ----------------------------
// Public API
// ----------------------------
esp_err_t network_init(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK INITIALIZATION STARTING");
    ESP_LOGI(TAG, "========================================");

    // [1] TinyUSB driver
    ESP_LOGI(TAG, "[1/7] Installing TinyUSB driver...");
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // [2] Initialize NCM EARLY (descriptors/callbacks exist before enumeration finishes)
    ESP_LOGI(TAG, "[2/7] Initializing USB NCM (early)...");
    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
        .on_init_callback = on_usb_net_init,
    };
    ret = tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start DOWN until stack is ready
    usb_set_link_state(false, "boot_init");

    // [3] CDC ACM (serial)
    ESP_LOGI(TAG, "[3/7] Initializing CDC-ACM...");
    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tusb_cdc_acm_init(&cdc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tusb_cdc_acm_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // [4] esp-netif
    ESP_LOGI(TAG, "[4/7] Creating esp-netif...");
    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_usb_ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM Server",
        .route_prio = 10
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free
    };

    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input
        }
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config
    };

    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_new returned NULL");
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, lwip_mac);

    // [5] DHCP config
    ESP_LOGI(TAG, "[5/7] Configuring DHCP server...");
    uint32_t lease_time = 1; // minutes
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           IP_ADDRESS_LEASE_TIME, &lease_time, sizeof(lease_time));

    dhcps_offer_t router_opt = OFFER_ROUTER;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           ROUTER_SOLICITATION_ADDRESS, &router_opt, sizeof(router_opt));

    dhcps_lease_t dhcp_lease;
    dhcp_lease.enable = true;
    IP4_ADDR(&dhcp_lease.start_ip, 192, 168, 7, 2);
    IP4_ADDR(&dhcp_lease.end_ip, 192, 168, 7, 10);
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           REQUESTED_IP_ADDRESS, &dhcp_lease, sizeof(dhcp_lease));

    // [6] Start netif + DHCP
    ESP_LOGI(TAG, "[6/7] Starting network interface...");
    esp_netif_action_start(s_netif, 0, 0, 0);
    event_log_record(EVT_NETIF_READY, NULL);
    s_stack_ready = true;

    // [7] Start watchdog task (self-heal)
    ESP_LOGI(TAG, "[7/7] Starting USB watchdog...");
    if (!s_usb_watchdog_task) {
        xTaskCreatePinnedToCore(
            usb_watchdog_task,
            "usb_watchdog",
            4096,
            NULL,
            10,
            &s_usb_watchdog_task,
            tskNO_AFFINITY
        );
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK INIT DONE");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 IP:    192.168.7.1");
    ESP_LOGI(TAG, "  DHCP Pool:   192.168.7.2 - 192.168.7.10");
    ESP_LOGI(TAG, "  USB MAC:     02:02:11:22:33:01");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Waiting for iOS/macOS to connect...");
    return ESP_OK;
}

void network_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts,
                       uint32_t *rx_bytes_out, uint32_t *tx_bytes_out)
{
    if (rx_pkts) *rx_pkts = s_rx_packets;
    if (tx_pkts) *tx_pkts = s_tx_packets;
    if (rx_bytes_out) *rx_bytes_out = s_rx_bytes;
    if (tx_bytes_out) *tx_bytes_out = s_tx_bytes;
}
