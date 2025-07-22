#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/pppos.h"
#include "lwip/lwip_napt.h"
#include "lwip/ip_addr.h"

#include "http_ui.h"
#include "modem.h"

wifi_config_t sta_config = {0};

static ppp_pcb *ppp = NULL;
struct netif ppp_netif;
static bool ppp_active = false;
static bool ppp_start_pending = false;
static volatile bool ppp_needs_cleanup = false;
static volatile int ppp_last_err = 0;

static const char *MODEM_TAG = "MODEM";
static const char *DNS_TAG = "DNS";
static const char *PPP_TAG = "PPP";
static const char *WIFI_TAG = "WIFI";


// dns_task
// Basically we use this to set up a custom DNS server so that we can do "captive portal" on very specific
// domains that sharkwire attempts to reach out to, like for activation, or the SharkWire Online home page
// for example
void dns_task(void *arg) {
    ESP_LOGI(DNS_TAG, "dns_task started on core %d", xPortGetCoreID());

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(DNS_TAG, "Socket error");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
   
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(DNS_TAG, "Bind error");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(DNS_TAG, "DNS server started (PORT: 53)");

    while (1) {
        uint8_t dns_buf[512];          
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);

        // Receive a DNS UDP packet from a client
        int len = recvfrom(sock, dns_buf, sizeof(dns_buf), 0, (struct sockaddr*)&client, &client_len);

        // Minimal DNS header is 12 bytes
        if (len < 12) continue;

        // Parse the queried domain name from the DNS packet
        char name[256];

        // DNS name starts at offset 12
        int i = 12;
        int pos = 0;

        while (dns_buf[i] != 0 && i < len) {
            int label_len = dns_buf[i++];
            // Sanity check
            if (label_len + i >= len) break;
            for (int j = 0; j < label_len && i < len; j++) {
                name[pos++] = dns_buf[i++];
            }
            name[pos++] = '.';
        }

        // remove last dot
        if (pos > 0) name[pos - 1] = '\0';
        else name[0] = '\0';

        ESP_LOGI(DNS_TAG, "DNS Query for: %s", name);

        // Check if the domain matches our special case
        if ((strcasecmp(name, "www.sharkwireonline.com") == 0) || (strcasecmp(name, "mail.sharkwire.com") == 0)) {

            // Build the response for things we want to resolve locally, like activation or home trap
            ESP_LOGI(DNS_TAG, "Custom DNS map");

            // Set flags: QR=1 (response), AA=1 (authoritative answer), RA=1 (recursion available)
            dns_buf[2] = 0x81;
            dns_buf[3] = 0x80;

            // Set answer count to 1
            dns_buf[7] = 1;

            // Start of answer section
            int offset = len;

            // Name: pointer back to query name at offset 12 (0xC00C)
            dns_buf[offset++] = 0xC0;
            dns_buf[offset++] = 0x0C;

            // Type A (0x0001), Class IN (0x0001)
            dns_buf[offset++] = 0x00; dns_buf[offset++] = 0x01;
            dns_buf[offset++] = 0x00; dns_buf[offset++] = 0x01;

            // TTL = 60 seconds
            dns_buf[offset++] = 0x00; dns_buf[offset++] = 0x00;
            dns_buf[offset++] = 0x00; dns_buf[offset++] = 60;

            // RDLENGTH = 4 bytes (IPv4)
            dns_buf[offset++] = 0x00; dns_buf[offset++] = 0x04;

            // RDATA: ESP32 AP IP address so we can handle activation/home mappings
            memcpy(&dns_buf[offset], &netif_ip4_addr(&ppp_netif)->addr, 4);
            offset += 4;

            // Send the response back to the client and hope for the best
            sendto(sock, dns_buf, offset, 0, (struct sockaddr*)&client, client_len);

        } else {
            // Build the response for everything else, which should go to a normal DNS server (8.8.8.8 for example)

            // Create a temporary UDP socket for forwarding
            int fwd_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (fwd_sock < 0) continue;

            struct sockaddr_in dns_server;
            dns_server.sin_family = AF_INET;
            dns_server.sin_port = htons(53);
            dns_server.sin_addr.s_addr = inet_addr("8.8.8.8");

            // Send the original DNS request to 8.8.8.8
            sendto(fwd_sock, dns_buf, len, 0, (struct sockaddr*)&dns_server, sizeof(dns_server));

            // Set a short timeout to wait for the upstream response
            struct timeval timeout = {2, 0};
            setsockopt(fwd_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            // Receive the upstream response
            uint8_t dns_reply[512];
            int dns_len = recvfrom(fwd_sock, dns_reply, sizeof(dns_reply), 0, NULL, NULL);
            if (dns_len > 0) {
                // Forward the reply directly to the original client
                sendto(sock, dns_reply, dns_len, 0, (struct sockaddr*)&client, client_len);
            }

            // Clean up forwarding socket
            close(fwd_sock);
        }
    }
}

// wifi_event_handler
// This is used for wifi event callbacks. It's not particularly needed, but later on it'll be useful for
// driving an onboard LED and/or pixel for showing WiFi status, for now it just prints
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(WIFI_TAG, "WiFi STA connected");
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(WIFI_TAG, "Got IP: %s", ip_str);
    }
}

// on_ppp_status
// This is the callback function for lwip PPP connection. I've opted to make any error
// just attempt a cleanup and fall back to AT cmd mode for redial, and it seems to work
// okay so far
static void on_ppp_status(ppp_pcb *pcb, int err_code, void *ctx) {
    if (err_code == PPPERR_NONE) {
        ESP_LOGI(PPP_TAG, "PPP connected.");
        ip_napt_enable(netif_ip4_addr(&ppp_netif)->addr, 1);
    } else {
        ESP_LOGI(PPP_TAG, "PPP disconnected (code=%d)", err_code);
        ppp_last_err = err_code;
        ppp_needs_cleanup = true;
    }
}

// ppp_output_cb
// This is used to deal with any data output that's going to take place over the PPP
// link. It basically sends PPP framed packets over the wire
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx) {
    uart_write_bytes(MODEM_UART, (const char*)data, len);
    return len;
}

// modem_task
// This is the main task for any communications we do. It registers the handlers for the event
// callback, and then starts a WiFi AP ( ESP32 <-> client ) on the specified SSID and gives it
// a default address of "192.168.4.1" for us to connect to and do setup/configuration of
// connection credentials.
//
// It then attempts to set up a STA connection ( ESP32 <-> router ) using any saved credentials,
// if there are any. If not it sets a temporary default.
//
// This task is also responsible for doing all WiFi and PPP connection setup, and handling transition
// between AT cmd mode, and PPP mode.
void modem_task(void *arg) {
    ESP_LOGI(MODEM_TAG, "modem_task started on core %d", xPortGetCoreID());

    uint8_t modem_buf[UART_BUFSIZE + 1];

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    esp_netif_create_default_wifi_sta();

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Try to load saved Wi-Fi credentials from NVS
    if (load_sta_credentials(&sta_config)) {
        ESP_LOGI(WIFI_TAG, "Loaded Wi-Fi credentials from NVS. SSID: %s", sta_config.sta.ssid);
    } else {
        // Fallback to defaults if nothing saved
        strcpy((char*)sta_config.sta.ssid, "None");
        strcpy((char*)sta_config.sta.password, "None");
        ESP_LOGI(WIFI_TAG, "No saved credentials, using default (SSID: \"None\" | PASS: \"None\")");
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "SharkShit64",
            .ssid_len = strlen("SharkShit64"),
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1
        }
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config);
    esp_wifi_start();
    esp_wifi_connect();

    xTaskCreate(dns_task, "dns_task", 8192, NULL, 8, NULL);
    xTaskCreate(http_ui_task, "http_ui_task", 8192, NULL, 8, NULL);

    // Enable NAT
    ip_napt_enable(IPADDR_ANY, 1);

    // These are the settings for the actual HW modem in ESP32
    uart_config_t uart_config = {
        .baud_rate = 19200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
        .rx_flow_ctrl_thresh = 122,
    };
    uart_driver_install(MODEM_UART, UART_BUFSIZE * 2, 0, 0, NULL, 0);
    uart_param_config(MODEM_UART, &uart_config);
    uart_set_pin(MODEM_UART, MODEM_TX, MODEM_RX, MODEM_RTS, MODEM_CTS);

    while (1) {
        int len = uart_read_bytes(MODEM_UART, modem_buf, UART_BUFSIZE, pdMS_TO_TICKS(50));

        // We set this flag earlier so that should for whatever reason we get disconnected, it
        // will set this flag and let it do proper tear down of PPP link and return to AT cmd mode.
        if (ppp_needs_cleanup) {
            ppp_needs_cleanup = false;
            ppp_active = false;
            if (ppp) {
                pppapi_close(ppp, 0);
                pppapi_free(ppp);
                ppp = NULL;
            }
            ESP_LOGI(PPP_TAG, "PPP closed. Returning to AT mode.");
            uart_write_bytes(MODEM_UART, "\r\nNO CARRIER\r\n", strlen("\r\nNO CARRIER\r\n"));
        }

        if (len > 0) {
            if (!ppp_active) {
                modem_buf[len] = '\0';
                ESP_LOGI(MODEM_TAG, "AT_CMD: %s", modem_buf);
                // Check if remote side is attempting to dial
                if (strstr((const char *)modem_buf, "ATD")) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    // It's at this point that we're "connected" and from here on only PPP comms happen
                    // while active
                    uart_write_bytes(MODEM_UART, "\r\nCONNECT\r\n", strlen("\r\nCONNECT\r\n"));
                    ppp_start_pending = true;
                } else {
                    // Everything else we just return "OK" as you would do for most generic AT cmds
                    uart_write_bytes(MODEM_UART, "\r\nOK\r\n", strlen("\r\nOK\r\n"));
                }
            } else if (ppp) {
                // This is used to deal with any data input that's going to take place over the PPP
                // link. It basically recvs PPP framed packets from over the wire
                pppos_input_tcpip(ppp, modem_buf, len);
            }
        }

        // Once PPP is active, do the PPP link setup
        if (ppp_start_pending && !ppp_active) {
            ppp_start_pending = false;
            ppp_active = true;

            ppp = pppapi_pppos_create(&ppp_netif, ppp_output_cb, on_ppp_status, NULL);

            // Set PAP auth, but don't really enforce it ( probably could, but shouldn't :D )
            ppp_set_auth(ppp, PPPAUTHTYPE_PAP, "test@sharkwire.com", "test");

            ip_addr_t our_ip, peer_ip, dnsserver;
            // So, we set the ESP32 PPP addr to 209.8.88.98 to trick the sharkwire into using our AP
            // as the secondary activation server where you set your username. This is done without DNS
            // and just makes it a lot easier to deal with. I'm not sure allowing it to be changed would
            // be of benefit because later on the SharkWire uses it to "Refresh User" and to "Add new user"
            // and the PPP link details are mostly hidden from the end user anyway.
            IP4_ADDR(&our_ip, 209,8,88,98);

            // This should be the SharkWire IP address ( The N64 IP itself )
            IP4_ADDR(&peer_ip, 209,8,88,99);

            // We basically hijack DNS here with our own custom DNS in order to provide a smoother user
            // experience by trapping things like the home page and activation1 ( again "activation2" is for 
            // setting username and does not use DNS to resolve the address, which is why we set our ESP32 
            // address to the address Sharkwire attempts to use so that it trucks it into thinking the ESP32
            // is the remote server ;) )
            IP4_ADDR(&dnsserver, 209,8,88,98);

            ppp_set_ipcp_ouraddr(ppp, &our_ip);
            ppp_set_ipcp_hisaddr(ppp, &peer_ip);
            ppp_set_ipcp_dnsaddr(ppp, 0, &dnsserver);
            ppp_set_ipcp_dnsaddr(ppp, 1, &dnsserver);
            //ppp_set_ipcp_dnsaddr(ppp, 2, &dnsserver);

            // Now connect PPP
            ESP_LOGI(PPP_TAG, "PPP local: " IPSTR ", peer: " IPSTR, IP2STR(&our_ip), IP2STR(&peer_ip));
            pppapi_connect(ppp, 0);
        }
    }
}
