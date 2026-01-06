/*
 * USB NCM Network Setup
 * Based on ESP-IDF sta2eth example pattern
 * Configures USB CDC-NCM device with DHCP server for iPhone/macOS connectivity
 *
 * ARCHITECTURE OVERVIEW:
 * ======================
 * This file sets up a complete network stack over USB:
 *
 *   [iPhone/Mac]  <--USB-->  [ESP32-S3 TinyUSB]  <-->  [esp-netif/lwIP]  <-->  [HTTP Server]
 *
 * Data flow (Host → ESP32):
 *   1. Host sends Ethernet frame over USB NCM
 *   2. TinyUSB NCM driver receives it, calls netif_recv_callback()
 *   3. We copy the buffer and pass to esp_netif_receive()
 *   4. lwIP processes the packet (ARP, IP, TCP, etc.)
 *   5. If it's HTTP to port 80, the HTTP server handles it
 *
 * Data flow (ESP32 → Host):
 *   1. lwIP generates a response packet
 *   2. esp-netif calls our netif_transmit() function
 *   3. We call tinyusb_net_send_sync() to send over USB
 *   4. Host receives the Ethernet frame
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
#include "tusb_cdc_acm.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/ip4_addr.h"

#include "network_setup.h"

static const char *TAG = "net";

// Static netif handle - the bridge between USB and lwIP
static esp_netif_t *s_netif = NULL;

// Packet counters for debugging
static uint32_t s_rx_packets = 0;
static uint32_t s_tx_packets = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_tx_bytes = 0;

/**
 * @brief Called when USB host connects and initializes NCM
 *
 * This callback fires when the host (iPhone/Mac) has successfully
 * enumerated the NCM device and is ready to send/receive Ethernet frames.
 * At this point, the host will start DHCP discovery.
 */
static void on_usb_net_init(void *ctx)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "USB HOST CONNECTED - NCM LINK UP");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Host has enumerated NCM device");
    ESP_LOGI(TAG, "  Waiting for DHCP DISCOVER from host...");
    ESP_LOGI(TAG, "  (Host will request IP via DHCP)");
}

/*
 * IP CONFIGURATION
 * ================
 * We use a private 192.168.7.0/24 network:
 *   - ESP32 (us):     192.168.7.1   (DHCP server, HTTP server)
 *   - Host (iPhone):  192.168.7.2-10 (assigned via DHCP)
 *   - Gateway:        192.168.7.254 (fake - prevents iOS routing internet through us)
 *
 * The fake gateway is important: if we set ourselves as gateway,
 * iOS might try to route ALL traffic through us, which we don't want.
 */
static const esp_netif_ip_info_t s_usb_ip_info = {
    .ip      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
    .gw      = { .addr = ESP_IP4TOADDR(192, 168, 7, 254) },  // Non-existent gateway
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
};

/**
 * @brief Free L2 buffer callback
 *
 * Called by esp-netif after it's done processing a received packet.
 * We allocated the buffer in netif_recv_callback(), so we free it here.
 */
static void l2_free(void *h, void *buffer)
{
    free(buffer);
}

/**
 * @brief Transmit callback: esp-netif/lwIP → USB Host
 *
 * Called whenever lwIP wants to send an Ethernet frame to the host.
 * This handles: ARP replies, DHCP offers, TCP/HTTP responses, etc.
 *
 * @param h      Driver handle (unused, we're a singleton)
 * @param buffer Ethernet frame to send (14-byte header + payload)
 * @param len    Total frame length
 */
static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    s_tx_packets++;
    s_tx_bytes += len;

    // Parse Ethernet header for logging
    uint8_t *eth = (uint8_t *)buffer;
    uint16_t ethertype = (eth[12] << 8) | eth[13];

    const char *type_str = "UNKNOWN";
    if (ethertype == 0x0800) type_str = "IPv4";
    else if (ethertype == 0x0806) type_str = "ARP";
    else if (ethertype == 0x86DD) type_str = "IPv6";

    ESP_LOGD(TAG, "TX #%lu: %zu bytes [%s] -> Host",
             (unsigned long)s_tx_packets, len, type_str);

    esp_err_t ret = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX FAILED: %s (is USB connected?)", esp_err_to_name(ret));
    }
    return ESP_OK;
}

/**
 * @brief Receive callback: USB Host → esp-netif/lwIP
 *
 * Called by TinyUSB when an Ethernet frame arrives from the host.
 * We must copy the buffer (TinyUSB reuses it) and pass to lwIP.
 *
 * This handles: ARP requests, DHCP discover/request, TCP/HTTP requests, etc.
 *
 * @param buffer Ethernet frame from host
 * @param len    Frame length
 * @param ctx    Context (unused)
 */
static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (!s_netif) {
        ESP_LOGW(TAG, "RX: Packet received but netif not ready, dropping");
        return ESP_OK;
    }

    s_rx_packets++;
    s_rx_bytes += len;

    // Parse Ethernet header for logging
    uint8_t *eth = (uint8_t *)buffer;
    uint16_t ethertype = (eth[12] << 8) | eth[13];

    const char *type_str = "UNKNOWN";
    if (ethertype == 0x0800) {
        type_str = "IPv4";
        // Could parse IP header for more detail (TCP/UDP, ports, etc.)
    } else if (ethertype == 0x0806) {
        type_str = "ARP";
    } else if (ethertype == 0x86DD) {
        type_str = "IPv6";
    }

    ESP_LOGD(TAG, "RX #%lu: %u bytes [%s] <- Host",
             (unsigned long)s_rx_packets, len, type_str);

    // Must copy - TinyUSB reuses this buffer
    void *buf_copy = malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "RX: malloc(%u) failed! Dropping packet", len);
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);

    // Hand off to lwIP for processing
    esp_err_t ret = esp_netif_receive(s_netif, buf_copy, len, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX: esp_netif_receive failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Initialize the complete USB NCM network stack
 *
 * This function sets up:
 *   1. TinyUSB driver (USB peripheral)
 *   2. CDC-ACM interface (serial logging)
 *   3. NCM interface (Ethernet over USB)
 *   4. esp-netif (bridges USB to lwIP)
 *   5. DHCP server (assigns IPs to connected hosts)
 */
esp_err_t network_init(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK INITIALIZATION STARTING");
    ESP_LOGI(TAG, "========================================");

    // =========================================================================
    // STEP 1: Install TinyUSB driver
    // =========================================================================
    // This initializes the ESP32-S3's USB peripheral in device mode.
    // After this, the chip is ready to enumerate when connected to a host.
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[1/7] Installing TinyUSB driver...");
    ESP_LOGI(TAG, "      - USB Device mode (not host)");
    ESP_LOGI(TAG, "      - Using internal PHY (no external USB chip)");

    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,  // ESP32-S3 has built-in USB PHY
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "      FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "      SUCCESS - USB peripheral initialized");

    // =========================================================================
    // STEP 2: Initialize CDC-ACM (Virtual Serial Port)
    // =========================================================================
    // This creates a virtual COM port alongside the network interface.
    // On macOS: appears as /dev/cu.usbmodemXXXX
    // Used for: ESP_LOG output so you can monitor via `screen` or `cat`
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[2/7] Initializing CDC-ACM serial interface...");
    ESP_LOGI(TAG, "      - Virtual COM port for log output");
    ESP_LOGI(TAG, "      - RX buffer: 256 bytes");

    tinyusb_config_cdcacm_t cdc_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 256,
        .callback_rx = NULL,                    // Not receiving commands
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,    // Could detect terminal connect
        .callback_line_coding_changed = NULL,   // Could detect baud rate change
    };
    ret = tusb_cdc_acm_init(&cdc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "      FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "      SUCCESS - Serial port ready");
    ESP_LOGI(TAG, "      (On macOS: /dev/cu.usbmodem*)");

    // =========================================================================
    // STEP 3: Initialize NCM (Network Control Model)
    // =========================================================================
    // NCM is a USB class for Ethernet-over-USB. It's natively supported by:
    //   - macOS (since 10.x)
    //   - iOS (since iOS 10ish, but better in iOS 13+)
    //   - Linux (cdc_ncm driver)
    //   - Windows (with driver, or RNDIS for better compat)
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[3/7] Initializing USB NCM (Ethernet over USB)...");
    ESP_LOGI(TAG, "      - MAC address: 02:02:11:22:33:01");
    ESP_LOGI(TAG, "        (02:xx = locally administered, won't conflict)");

    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,  // Called when host sends us data
        .on_init_callback = on_usb_net_init,      // Called when host connects
    };

    ret = tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "      FAILED: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "      SUCCESS - NCM device registered");
    ESP_LOGI(TAG, "      (Waiting for USB host to enumerate us...)");

    // =========================================================================
    // STEP 4: Create esp-netif configuration
    // =========================================================================
    // esp-netif is ESP-IDF's network interface abstraction layer.
    // It connects our USB driver to the lwIP TCP/IP stack.
    //
    // We configure it with:
    //   - DHCP_SERVER flag: we'll assign IPs to connecting hosts
    //   - AUTOUP flag: bring interface up automatically
    //   - Static IP: 192.168.7.1
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[4/7] Creating esp-netif configuration...");
    ESP_LOGI(TAG, "      - Interface key: 'usb_ncm'");
    ESP_LOGI(TAG, "      - Static IP: 192.168.7.1/24");
    ESP_LOGI(TAG, "      - DHCP server: ENABLED");
    ESP_LOGI(TAG, "      - lwIP MAC: 02:02:11:22:33:02");
    ESP_LOGI(TAG, "        (Different from USB MAC for routing)");

    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_usb_ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM Server",
        .route_prio = 10  // Lower than WiFi (100) by default
    };

    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,              // Non-NULL required; we're singleton
        .transmit = netif_transmit,       // Our TX function
        .driver_free_rx_buffer = l2_free  // Free after RX processing
    };

    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,   // Standard Ethernet init
            .input_fn = ethernetif_input  // Standard Ethernet input
        }
    };

    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config
    };

    // =========================================================================
    // STEP 5: Create the network interface
    // =========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[5/7] Creating network interface...");

    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "      FAILED: esp_netif_new returned NULL");
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, lwip_mac);
    ESP_LOGI(TAG, "      SUCCESS - Interface created");

    // =========================================================================
    // STEP 6: Configure DHCP server
    // =========================================================================
    // The DHCP server automatically assigns IPs to connecting hosts.
    // When iPhone connects, it will:
    //   1. Send DHCP DISCOVER (broadcast)
    //   2. We reply with DHCP OFFER (192.168.7.2)
    //   3. iPhone sends DHCP REQUEST
    //   4. We reply with DHCP ACK
    //   5. iPhone now has IP 192.168.7.2 and can reach us at 192.168.7.1
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[6/7] Configuring DHCP server...");

    // Short lease time for faster reconnection after unplug/replug
    uint32_t lease_time = 1;  // 1 minute
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           IP_ADDRESS_LEASE_TIME, &lease_time, sizeof(lease_time));
    ESP_LOGI(TAG, "      - Lease time: %lu minute(s)", (unsigned long)lease_time);

    // Advertise router option (iOS requires this)
    dhcps_offer_t router_opt = OFFER_ROUTER;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           ROUTER_SOLICITATION_ADDRESS, &router_opt, sizeof(router_opt));
    ESP_LOGI(TAG, "      - Router option: ENABLED (required for iOS)");

    // Configure IP pool
    dhcps_lease_t dhcp_lease;
    dhcp_lease.enable = true;
    IP4_ADDR(&dhcp_lease.start_ip, 192, 168, 7, 2);
    IP4_ADDR(&dhcp_lease.end_ip, 192, 168, 7, 10);
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                           REQUESTED_IP_ADDRESS, &dhcp_lease, sizeof(dhcp_lease));
    ESP_LOGI(TAG, "      - IP pool: 192.168.7.2 - 192.168.7.10");
    ESP_LOGI(TAG, "      - Max clients: 9");

    // =========================================================================
    // STEP 7: Start the interface
    // =========================================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "[7/7] Starting network interface...");

    esp_netif_action_start(s_netif, 0, 0, 0);

    ESP_LOGI(TAG, "      SUCCESS - Interface is UP");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK INITIALIZATION COMPLETE");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Configuration Summary:");
    ESP_LOGI(TAG, "  ESP32 IP:      192.168.7.1");
    ESP_LOGI(TAG, "  Netmask:       255.255.255.0");
    ESP_LOGI(TAG, "  DHCP Pool:     192.168.7.2 - 192.168.7.10");
    ESP_LOGI(TAG, "  USB MAC:       02:02:11:22:33:01");
    ESP_LOGI(TAG, "  lwIP MAC:      02:02:11:22:33:02");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Waiting for USB host connection...");
    ESP_LOGI(TAG, "(Connect iPhone/Mac via USB-C cable)");
    ESP_LOGI(TAG, "");

    return ESP_OK;
}

/**
 * @brief Get network statistics (for debugging)
 */
void network_get_stats(uint32_t *rx_pkts, uint32_t *tx_pkts,
                       uint32_t *rx_bytes_out, uint32_t *tx_bytes_out)
{
    if (rx_pkts) *rx_pkts = s_rx_packets;
    if (tx_pkts) *tx_pkts = s_tx_packets;
    if (rx_bytes_out) *rx_bytes_out = s_rx_bytes;
    if (tx_bytes_out) *tx_bytes_out = s_tx_bytes;
}
