#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"

#include "esp_hidh.h"
#include "esp_hid_gap.h"

#include "gpio_keyboard.h"

static const char *BLE_HOST_TAG = "BLE_HOST";

static bool connected = false;

static const uint8_t hid_to_scancode[128] = {
    // Lowercase letters (HID 0x04–0x1D)
    // a           // b           // c           // d
    [0x04] = 0x1C, [0x05] = 0x32, [0x06] = 0x21, [0x07] = 0x23,
    // e           // f           // g           // h
    [0x08] = 0x24, [0x09] = 0x2B, [0x0A] = 0x34, [0x0B] = 0x33,
    // i           // j           // k           // l 
    [0x0C] = 0x43, [0x0D] = 0x3B, [0x0E] = 0x42, [0x0F] = 0x4B,
    // m           // n           // o           // p
    [0x10] = 0x3A, [0x11] = 0x31, [0x12] = 0x44, [0x13] = 0x4D,
    // q           // r           // s           // t
    [0x14] = 0x15, [0x15] = 0x2D, [0x16] = 0x1B, [0x17] = 0x2C,
    // u           // v           // w           // x
    [0x18] = 0x3C, [0x19] = 0x2A, [0x1A] = 0x1D, [0x1B] = 0x22,
    // y           // z
    [0x1C] = 0x35, [0x1D] = 0x1A,

    // Digits (HID 0x1E–0x27)
    [0x1E] = 0x16, // 1
    [0x1F] = 0x1E, // 2
    [0x20] = 0x26, // 3
    [0x21] = 0x25, // 4
    [0x22] = 0x2E, // 5
    [0x23] = 0x36, // 6
    [0x24] = 0x3D, // 7
    [0x25] = 0x3E, // 8
    [0x26] = 0x46, // 9
    [0x27] = 0x45, // 0

    // Space, symbols (HID 0x2C+)
    [0x2C] = 0x29, // Space
    [0x2D] = 0x4E, // -
    [0x2E] = 0x55, // =
    [0x2F] = 0x54, // [
    [0x30] = 0x5B, // ]
    [0x31] = 0x5D, //
    [0x33] = 0x4C, // ;
    [0x34] = 0x52, // '
    [0x35] = 0x0E, // `
    [0x36] = 0x41, // ,
    [0x37] = 0x49, // .
    [0x38] = 0x4A, // /

    // Control keys
    [0x28] = 0x5A, // Enter
    [0x29] = 0x76, // ESC
    [0x2B] = 0x0D, // Tab
    [0x2A] = 0x66, // Backspace

    // Arrows
    [0x4F] = 0x74, // Right
    [0x50] = 0x6B, // Left
    [0x51] = 0x72, // Down
    [0x52] = 0x75, // Up
};

char *bda2str(uint8_t *bda, char *str, size_t size)
{
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }
    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

uint8_t last_keys[MAX_KEYS] = {0};

void handle_input(const uint8_t *data, size_t len) {
    const uint8_t *keys = &data[2]; // skip modifier + reserved

    // For each new keycode, see if it's not in the previous state
    for (int i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = keys[i];
        if (key == 0) continue;

        bool is_new = true;
        for (int j = 0; j < MAX_KEYS; ++j) {
            if (key == last_keys[j]) {
                is_new = false;
                break;
            }
        }

        if (is_new) {
            queue_scancode(hid_to_scancode[key]);
        }
    }

    // Update last_keys
    memcpy(last_keys, keys, MAX_KEYS);
}

void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {

    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            connected = true;

            // When connected, set GPIO up
            init_gpio_keyboard();

            ESP_LOGI(BLE_HOST_TAG, "Device connected.");
        } else {
            ESP_LOGE(BLE_HOST_TAG, "Failed to open device.");
            connected = false;
            xTaskCreate(&ble_task, "ble_scan_retry", 8192, NULL, 5, NULL);
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        if (bda) {
            ESP_LOGI(BLE_HOST_TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda),
            param->battery.level);
        }
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
        if (bda) {
            ESP_LOGI(BLE_HOST_TAG, ESP_BD_ADDR_STR " INPUT: %8s, MAP: %2u, ID: %3u, Len: %d, Data:", 
            ESP_BD_ADDR_HEX(bda), esp_hid_usage_str(param->input.usage), param->input.map_index, 
            param->input.report_id, param->input.length);
            
            handle_input(param->input.data, param->input.length);

            ESP_LOG_BUFFER_HEX(BLE_HOST_TAG, param->input.data, param->input.length);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        ESP_LOGI(BLE_HOST_TAG, "Device disconnected.");
        connected = false;
        xTaskCreate(&ble_task, "ble_scan_retry", 8192, NULL, 5, NULL);
        break;
    }

    default:
        break;
    }
}

void ble_task(void *args)
{
    // Step 1: Check for bonded devices
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > 0) {
        esp_ble_bond_dev_t bonded_dev_list[dev_num];
        if (esp_ble_get_bond_device_list(&dev_num, bonded_dev_list) == ESP_OK) {
            ESP_LOGI(BLE_HOST_TAG, "Found %d bonded device(s)", dev_num);

            esp_bd_addr_t *bda = &bonded_dev_list[0].bd_addr; // Only one supported

            ESP_LOGI(BLE_HOST_TAG, "Trying to connect to bonded device: " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(*bda));

            // Attempt to open it (use BLE public address type)
            esp_hidh_dev_open(*bda, ESP_HID_TRANSPORT_BLE, BLE_ADDR_TYPE_PUBLIC);

            vTaskDelete(NULL);
            return;
        }
    }

    // Step 2: Fall back to scan if no bonded devices
    while (!connected) {
        size_t results_len = 0;
        esp_hid_scan_result_t *results = NULL;

        ESP_LOGI(BLE_HOST_TAG, "Scanning for devices...");
        esp_hid_scan(BLE_SCAN_DURATION_SECONDS, &results_len, &results);

        if (results_len) {
            esp_hid_scan_result_t *r = results;
            esp_hid_scan_result_t *cr = NULL;

            while (r) {
                ESP_LOGI(BLE_HOST_TAG, "  %s: " ESP_BD_ADDR_STR ", RSSI: %d, USAGE: %s, NAME: %s",
                         (r->transport == ESP_HID_TRANSPORT_BLE) ? "BLE" : "BT ",
                         ESP_BD_ADDR_HEX(r->bda),
                         r->rssi,
                         esp_hid_usage_str(r->usage),
                         r->name ? r->name : "");

                if (r->transport == ESP_HID_TRANSPORT_BLE) {
                    cr = r;
                }
                r = r->next;
            }

            if (cr) {
                ESP_LOGI(BLE_HOST_TAG, "Trying to open scanned device...");
                esp_hidh_dev_open(cr->bda, cr->transport, cr->ble.addr_type);
                esp_hid_scan_results_free(results);
                break;
            }

            esp_hid_scan_results_free(results);
        } else {
            ESP_LOGI(BLE_HOST_TAG, "No devices found. Retrying in 10s...");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    vTaskDelete(NULL);
}