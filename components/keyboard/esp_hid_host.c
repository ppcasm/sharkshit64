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

#include "esp_hid_host.h"
#include "gpio_keyboard.h"

static const char *BLE_HOST_TAG = "BLE_HOST";

static uint8_t last_keys[MAX_KEYS] = {0};
static uint8_t held_ticks[256] = {0};
static TaskHandle_t keyboard_repeat_task_handle = NULL;

static bool bda_valid = false;
static bool ble_connected = false;
static esp_bd_addr_t last_bonded_bda = {0};
char current_ble_manufacturer[64] = "Unknown";

static int ble_battery_level = -1;

static bool current_shift = false;
static bool shift_sent = false;

static bool still_held[KB_BUFFER_SIZE];

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

    [0x3A] = 0x05, // F1
    [0x3B] = 0x06, // F2
    [0x3C] = 0x04, // F3
    [0x3D] = 0x0C, // F4
    [0x3E] = 0x03, // F5
    [0x3F] = 0x0B, // F6
    [0x40] = 0x83, // F7
    [0x41] = 0x0A, // F8
    [0x42] = 0x01, // F9
    [0x43] = 0x09, // F10
    [0x44] = 0x78, // F11
    [0x45] = 0x07, // F12

    [0x4C] = 0x71, // Del
};

static const uint8_t hid_to_scancode_shifted[128] = {
    [0x04] = 0x1C, [0x05] = 0x32, [0x06] = 0x21, [0x07] = 0x23,
    [0x08] = 0x24, [0x09] = 0x2B, [0x0A] = 0x34, [0x0B] = 0x33,
    [0x0C] = 0x43, [0x0D] = 0x3B, [0x0E] = 0x42, [0x0F] = 0x4B,
    [0x10] = 0x3A, [0x11] = 0x31, [0x12] = 0x44, [0x13] = 0x4D,
    [0x14] = 0x15, [0x15] = 0x2D, [0x16] = 0x1B, [0x17] = 0x2C,
    [0x18] = 0x3C, [0x19] = 0x2A, [0x1A] = 0x1D, [0x1B] = 0x22,
    [0x1C] = 0x35, [0x1D] = 0x1A,
    [0x1E] = 0x16, [0x1F] = 0x1E, [0x20] = 0x26, [0x21] = 0x25,
    [0x22] = 0x2E, [0x23] = 0x36, [0x24] = 0x3D, [0x25] = 0x3E,
    [0x26] = 0x46, [0x27] = 0x45,
    [0x2D] = 0x4E, [0x2E] = 0x55, [0x2F] = 0x54,
    [0x30] = 0x5B, [0x31] = 0x5D, [0x33] = 0x4C, [0x34] = 0x52,
    [0x35] = 0x0E, [0x36] = 0x41, [0x37] = 0x49, [0x38] = 0x4A,

    [0x3A] = 0x05, // F1
    [0x3B] = 0x06, // F2
    [0x3C] = 0x04, // F3
    [0x3D] = 0x0C, // F4
    [0x3E] = 0x03, // F5
    [0x3F] = 0x0B, // F6
    [0x40] = 0x83, // F7
    [0x41] = 0x0A, // F8
    [0x42] = 0x01, // F9
    [0x43] = 0x09, // F10
    [0x44] = 0x78, // F11
    [0x45] = 0x07, // F12

    [0x4C] = 0x71, // Del
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

void keyboard_tick() {
    memset(still_held, 0, KB_BUFFER_SIZE);

    // Track which keys are still held
    for (int i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = last_keys[i];
        if (key == 0) continue;
        still_held[key] = true;

        // Handle shift key modifiers
        uint8_t scancode = current_shift && hid_to_scancode_shifted[key]
                         ? hid_to_scancode_shifted[key]
                         : hid_to_scancode[key];

        if (held_ticks[key] == 0) {
            // Handle first press and inject shift modifier if applicable, flag as sent
            if (current_shift && !shift_sent) {
                queue_scancode(0x12);
                shift_sent = true;
            }
            // Send extended for Del
            if (scancode == 0x71) {
                queue_scancode(0xE0);
            }
            queue_scancode(scancode);

        // Handle repeating
        } else if (held_ticks[key] >= REPEAT_DELAY_TICKS &&
                  (held_ticks[key] - REPEAT_DELAY_TICKS) % REPEAT_INTERVAL_TICKS == 0) {
            // Send extended for Del
            if (scancode == 0x71) {
                queue_scancode(0xE0);
            }
            queue_scancode(scancode);
        }

        held_ticks[key]++;
    }

    // Reset tick counter for keys no longer held
    for (int i = 0; i < KB_BUFFER_SIZE; ++i) {
        // Here we will need to handle BREAK codes for shifted keys, because otherwise the N64
        // will assume all keys afterwards are shifted as it uses the BREAK to delimit shift depressed
        if (!still_held[i] && held_ticks[i] != 0) {
            uint8_t scancode = current_shift && hid_to_scancode_shifted[i]
                ? hid_to_scancode_shifted[i]
                : hid_to_scancode[i];
            // BREAK prefix
            queue_scancode(0xF0);
            queue_scancode(scancode);
            held_ticks[i] = 0;
        }
    }

    if (!current_shift && shift_sent) {
        queue_scancode(0xF0);
        queue_scancode(0x12);
        shift_sent = false;
    }
}

void handle_input(const uint8_t *data, size_t len) {
    // Alright this shit is kind of a mess right now. Some BLE keyboards seem to
    // have different key queue sizes, and so until I figure out a better way to handle that
    // I'll just assume the payload offset based off its size for now (I'm sure there's a better
    // way, I just haven't fully researched the protocol yet)

    // For now the hack is basically if its length is 8, then the scancode offset starts at offset
    // 2, and if it's less than that the scancode data starts at offset 1, with potentially the modifier
    // bits offset starting at 1 for len of 8 and and 0 for length of less than 8, but I guess we will
    // find out the hard way :D
    int sc_offset = 2;
    if (len < 8) sc_offset = 1;
    const uint8_t *keys = &data[sc_offset];

    memset(last_keys, 0, MAX_KEYS);

    if (len >= 1) {
        // This might need to be fixed later depending on what I figure out about where the modifiers
        // are stored based on the payload length theory from earlier
        uint8_t mods = data[0];

        // check/set left and right shift modifier bits
        current_shift = (mods & (1 << 1)) || (mods & (1 << 5));
    }

    for (int i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = keys[i];
        if (key == 0) continue;
        last_keys[i] = key;
    }
}

void keyboard_repeat_task(void *arg) {
    while (1) {
        keyboard_tick();
        vTaskDelay(pdMS_TO_TICKS(KB_TICK_RATE));
    }
}

int get_ble_battery_level(void) {
    return ble_battery_level;
}

bool get_bonded_device_address(esp_bd_addr_t out_bda) {
    if (!bda_valid) return false;
    memcpy(out_bda, last_bonded_bda, sizeof(esp_bd_addr_t));
    return true;
}

bool is_ble_connected(void) {
    return ble_connected;
}

void ble_set_manufacturer(const char *value) {
    strncpy(current_ble_manufacturer,
            value ? value : "Unknown",
            sizeof(current_ble_manufacturer) - 1);
    current_ble_manufacturer[sizeof(current_ble_manufacturer) - 1] = '\0';
}

const char *get_ble_manufacturer(void) {
    return current_ble_manufacturer;
}

bool unbond_ble_device(void) {
    esp_bd_addr_t bda;
    int dev_num = esp_ble_get_bond_device_num();

    esp_ble_bond_dev_t bonded_dev_list[dev_num];
    
    if (esp_ble_get_bond_device_list(&dev_num, bonded_dev_list) == ESP_OK && dev_num > 0) {
        memcpy(bda, bonded_dev_list[0].bd_addr, sizeof(esp_bd_addr_t));
        esp_err_t err = esp_ble_remove_bond_device(bda);
        if (err == ESP_OK) {
            ESP_LOGI(BLE_HOST_TAG, "Unbonded BLE device: %02X:%02X:%02X:%02X:%02X:%02X",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            return true;
        } else {
            ESP_LOGI(BLE_HOST_TAG, "Failed to remove bonded device: %s", esp_err_to_name(err));
        }
    }
    return false;
}

void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {

    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            ble_connected = true;

            // When connected, set GPIO up
            init_gpio_keyboard();

            if (keyboard_repeat_task_handle == NULL) {
            xTaskCreate(keyboard_repeat_task, "kbd_repeat", 8192, NULL, 5, &keyboard_repeat_task_handle);
            }

            esp_hidh_dev_t *dev = param->open.dev;
            ble_set_manufacturer(esp_hidh_dev_manufacturer_get(dev));

            ESP_LOGI(BLE_HOST_TAG, "Device connected.");
        } else {  
            ESP_LOGI(BLE_HOST_TAG, "Failed to open device (status: 0x%02x)", param->open.status);  

            if (keyboard_repeat_task_handle != NULL) {
                vTaskDelete(keyboard_repeat_task_handle);
                keyboard_repeat_task_handle = NULL;
            }

            ble_connected = false;
            vTaskDelay(pdMS_TO_TICKS(5000));
            xTaskCreate(&ble_task, "ble_scan_retry", 8192, NULL, 5, NULL);
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        if (bda) {
            ESP_LOGI(BLE_HOST_TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda),
            param->battery.level);

            ble_battery_level = param->battery.level;
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

        if (keyboard_repeat_task_handle != NULL) {
            vTaskDelete(keyboard_repeat_task_handle);
            keyboard_repeat_task_handle = NULL;
        }

        ble_connected = false;
        xTaskCreate(&ble_task, "ble_scan_retry", 8192, NULL, 5, NULL);
        break;
    }

    default:
        break;
    }
}

void ble_task(void *args)
{
    // Check for bonded devices
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num > 0) {
        esp_ble_bond_dev_t bonded_dev_list[dev_num];
        if (esp_ble_get_bond_device_list(&dev_num, bonded_dev_list) == ESP_OK) {
            esp_bd_addr_t *bda = &bonded_dev_list[0].bd_addr;

            memcpy(last_bonded_bda, bonded_dev_list[0].bd_addr, sizeof(esp_bd_addr_t));
            bda_valid = true;

            while (!ble_connected) {
                ESP_LOGI(BLE_HOST_TAG, "Trying to connect to bonded device: " ESP_BD_ADDR_STR,
                ESP_BD_ADDR_HEX(*bda));

                esp_hidh_dev_open(*bda, ESP_HID_TRANSPORT_BLE, BLE_ADDR_TYPE_PUBLIC);

                // Wait for open result (success sets 'ble_connected' in hidh_callback)
                for (int i = 0; i < 20 && !ble_connected; ++i) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (!ble_connected) {
                    ESP_LOGI(BLE_HOST_TAG, "Bonded connection failed. Retrying in 1s...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }

            vTaskDelete(NULL);
            return;
        }
    }

    // Fall back to scan if no bonded devices
    while (!ble_connected) {
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