/*
 * USB NCM Network Setup
 * Based on ESP-IDF sta2eth example pattern
 * Configures USB CDC-NCM device with DHCP server for iPhone/macOS connectivity
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"

#include "tinyusb.h"
#include "tinyusb_net.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"

#include "network_setup.h"

static const char *TAG = "usb_ncm_network";

// Static netif handle
static esp_netif_t *s_netif = NULL;

/**
 * @brief Called when USB host connects and initializes NCM
 */
static void on_usb_net_init(void *ctx)
{
    ESP_LOGI(TAG, "USB host connected - NCM interface initialized");
}

// Custom IP configuration for USB network (192.168.7.1/24)
// Gateway set to .254 (doesn't exist) so iOS won't route internet through it
static const esp_netif_ip_info_t s_usb_ip_info = {
    .ip      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
    .gw      = { .addr = ESP_IP4TOADDR(192, 168, 7, 254) },  // Non-existent gateway
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

/**
 * @brief Free L2 buffer callback
 */
static void l2_free(void *h, void *buffer)
{
    free(buffer);
}

/**
 * @brief Transmit callback for esp-netif -> USB
 */
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    esp_err_t ret = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send buffer to USB: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

/**
 * @brief Receive callback: USB -> esp-netif
 * Called by TinyUSB when data is received from host
 */
static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_netif) {
        void *buf_copy = malloc(len);
        if (!buf_copy) {
            ESP_LOGE(TAG, "Failed to allocate buffer for received packet");
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf_copy, buffer, len);
        return esp_netif_receive(s_netif, buf_copy, len, NULL);
    }
    return ESP_OK;
}

esp_err_t network_init(void)
{
    ESP_LOGI(TAG, "Initializing USB NCM network interface");

    // 1. Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "TinyUSB driver installed");

    // 2. Configure NCM device
    // Use locally administered MAC address for USB interface
    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
        .on_init_callback = on_usb_net_init,
    };

    ret = tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TinyUSB NCM: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "TinyUSB NCM device initialized");

    // 3. Create esp-netif configuration
    // Use different MAC for lwip interface (required for proper routing)
    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    // Base configuration: DHCP server enabled, auto-up
    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_usb_ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM Server",
        .route_prio = 10
    };

    // Driver configuration: point to static TX/free functions
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,  // Must be non-NULL, USB NCM is singleton
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free
    };

    // Network stack config: use ethernet lwip functions
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

    // 4. Create netif
    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create esp-netif");
        return ESP_FAIL;
    }

    // Set MAC address for lwip interface
    esp_netif_set_mac(s_netif, lwip_mac);

    // 5. Configure DHCP server options
    // Set minimum lease time (1 minute) for faster reconnection
    uint32_t lease_time = 1;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           IP_ADDRESS_LEASE_TIME, &lease_time, sizeof(lease_time));

    // Enable router option - iOS needs this for the interface to be active
    // Note: On macOS, set WiFi to higher priority in System Settings > Network if needed
    dhcps_offer_t router_opt = OFFER_ROUTER;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           ROUTER_SOLICITATION_ADDRESS, &router_opt, sizeof(router_opt));

    // Configure DHCP pool (192.168.7.2 - 192.168.7.10)
    dhcps_lease_t dhcp_lease;
    dhcp_lease.enable = true;
    IP4_ADDR(&dhcp_lease.start_ip, 192, 168, 7, 2);
    IP4_ADDR(&dhcp_lease.end_ip, 192, 168, 7, 10);
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           REQUESTED_IP_ADDRESS, &dhcp_lease, sizeof(dhcp_lease));

    // 6. Start the interface
    esp_netif_action_start(s_netif, 0, 0, 0);

    ESP_LOGI(TAG, "USB NCM network ready:");
    ESP_LOGI(TAG, "  IP: 192.168.7.1");
    ESP_LOGI(TAG, "  DHCP pool: 192.168.7.2 - 192.168.7.10");
    ESP_LOGI(TAG, "  USB MAC: 02:02:11:22:33:01");
    ESP_LOGI(TAG, "  LwIP MAC: 02:02:11:22:33:02");

    return ESP_OK;
}
