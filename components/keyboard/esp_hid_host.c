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
#include "keyboard.h"

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

static uint8_t still_held_bits[32] = {0};

#define IS_HELD(k)   (still_held_bits[(k) >> 3] & (1 << ((k) & 7)))
#define SET_HELD(k)  (still_held_bits[(k) >> 3] |= (1 << ((k) & 7)))
#define CLR_HELD(k)  (still_held_bits[(k) >> 3] &= ~(1 << ((k) & 7)))

// This is used to do HID to PS/2 scancode mapping with shifted tables
//
// For now we will just waste some flash space and keep 2 tables with
// the same data in case we want to remap something in the future
static const uint8_t scancode_table[2][128] = {
        [0x00] = {
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
        [0x2C] = 0x29, [0x2D] = 0x4E, [0x2E] = 0x55, [0x2F] = 0x54,
        [0x30] = 0x5B, [0x31] = 0x5D, [0x33] = 0x4C, [0x34] = 0x52,
        [0x35] = 0x0E, [0x36] = 0x41, [0x37] = 0x49, [0x38] = 0x4A,
        [0x28] = 0x5A, [0x29] = 0x76, [0x2B] = 0x0D, [0x2A] = 0x66,
        [0x4F] = 0x74, [0x50] = 0x6B, [0x51] = 0x72, [0x52] = 0x75,
        [0x3A] = 0x05, [0x3B] = 0x06, [0x3C] = 0x04, [0x3D] = 0x0C,
        [0x3E] = 0x03, [0x3F] = 0x0B, [0x40] = 0x83, [0x41] = 0x0A,
        [0x42] = 0x01, [0x43] = 0x09, [0x44] = 0x78, [0x45] = 0x07,
        [0x4C] = 0x71
    },
        [0x01] = {
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
        [0x3A] = 0x05, [0x3B] = 0x06, [0x3C] = 0x04, [0x3D] = 0x0C,
        [0x3E] = 0x03, [0x3F] = 0x0B, [0x40] = 0x83, [0x41] = 0x0A,
        [0x42] = 0x01, [0x43] = 0x09, [0x44] = 0x78, [0x45] = 0x07,
        [0x4C] = 0x71
    }
};

// bda2str
// This takes the BLE device address and turns it into a 
// proper string format
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

// keyboard_tick
// This is ran from the keyboard_repeat_task and does most of the heavy lifting of processing
// the inputs by moving through the keys that get buffered through BLE input, and then building
// a proper PS/2 scancode format with MAKE/BREAK and shifted codes, along with the outlier for
// the Del key, which uses the extended code format, and places them in the scancode queue that
// gets processed in 'gpio_keyboard.c'
//
// It runs at an interval from a separate task (keyboard_repeat_task) in such a way because BLE 
// devices don't actually handle any kind of interval control, so we must create our own to mimic
// key repeating, and these timings can be set in 'keyboard.h' to allow customizations
//
// It should also be noted that Sharkwire expects BREAK codes to be sent with shifted codes
// or it'll stick in a shifted state as it uses 0xF0 BREAK to delimit the depress of the
// shift key
void keyboard_tick() {
    memset(still_held_bits, 0, sizeof(still_held_bits));

    for (int i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = last_keys[i];
        if (key == 0) continue;
        SET_HELD(key);

        uint8_t scancode = scancode_table[current_shift][key];

        if (held_ticks[key] == 0) {
            if (current_shift && !shift_sent) {
                queue_scancode(0x12);
                shift_sent = true;
            }
            if (scancode == 0x71) queue_scancode(0xE0);
            queue_scancode(scancode);
        } else if (held_ticks[key] == REPEAT_DELAY_TICKS ||
                  ((held_ticks[key] > REPEAT_DELAY_TICKS) &&
                  ((held_ticks[key] - REPEAT_DELAY_TICKS) & (REPEAT_INTERVAL_TICKS - 1)) == 0)) {
            if (scancode == 0x71) queue_scancode(0xE0);
            queue_scancode(scancode);
        }

        held_ticks[key]++;
    }

    for (int i = 0; i < 128; ++i) {
        if (!IS_HELD(i) && held_ticks[i] != 0) {
            uint8_t scancode = scancode_table[current_shift][i];
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

// handle_input
// This is called from the BLE HIDH input event and takes in the BLE input "buffered keys"
// and sets the data, as well as the shift masking that gets processed during the keyboard_tick
// function that runs from the keyboard_repeat_task effectively allowing this to fill the correct
// structures to allow input to be processed
void handle_input(const uint8_t *data, size_t len) {
    int sc_offset = len < 8 ? 1 : 2;
    const uint8_t *keys = &data[sc_offset];

    for (int i = 0; i < MAX_KEYS; ++i) last_keys[i] = 0;

    if (len >= 1) {
        uint8_t mods = data[0];
        current_shift = (mods & (1 << 1)) || (mods & (1 << 5));
    }

    for (int i = 0; i < MAX_KEYS; ++i) {
        uint8_t key = keys[i];
        if (key) last_keys[i] = key;
    }
}

// keyboard_repeat_task
// This is responsible for allowing the BLE key to be repeated if held, and
// helps us time it at a specific rate
void keyboard_repeat_task(void *arg) {
    while (1) {
        keyboard_tick();
        vTaskDelay(pdMS_TO_TICKS(KB_TICK_RATE));
    }
}

// get_ble_battery_level
// We use this to return the BLE battery level is applicable, and it's mainly
// used in the 'http_ui" component to show battery level in BLE status
int get_ble_battery_level(void) {
    return ble_battery_level;
}

// get_bonded_device_address
// Obviously gets the device address of our (1) allowed bonded device, again
// it's mainly used in the 'http_ui' component to display the address of the
// currently bonded device
bool get_bonded_device_address(esp_bd_addr_t out_bda) {
    if (!bda_valid) return false;
    memcpy(out_bda, last_bonded_bda, sizeof(esp_bd_addr_t));
    return true;
}

// is_ble_connected
// This returns connected state, mostly to expose it to 'http_ui' so that we
// can show whether a BLE device is currently and actively connected
bool is_ble_connected(void) {
    return ble_connected;
}

// ble_set_manufacturer
// This allows us to set the manufacturer when the data comes through so it
// can be ready for the 'http_ui' BLE status, where it's displayed
void ble_set_manufacturer(const char *value) {
    strncpy(current_ble_manufacturer,
            value ? value : "Unknown",
            sizeof(current_ble_manufacturer) - 1);
    current_ble_manufacturer[sizeof(current_ble_manufacturer) - 1] = '\0';
}

// get_ble_manufacturer
// This is where we get the manufacturer that's set from the 'ble_set_manufacturer' function
// and is used to display BLE manufacturer in the 'http_ui' component
const char *get_ble_manufacturer(void) {
    return current_ble_manufacturer;
}

// unbond_ble_device
// This is used to unpair/unbond our single supported device that's saved in NVS, and this
// gets exposed through 'http_ui' to easily allow us to do this through a webUI button
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

// hidh_callback
// Part of the Bluedroid stack that handles OPEN/INPUT/CLOSE/BATTERY and a few other events
// that let you setup how you want these events to be handled
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
            xTaskCreate(keyboard_repeat_task, "keyboard_repeat_task", KBD_REPEAT_TASK_SIZE, NULL, KBD_REPEAT_TASK_PRI, &keyboard_repeat_task_handle);
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
            xTaskCreate(&ble_task, "ble_scan_retry", BLE_TASK_SIZE, NULL, BLE_TASK_PRI, NULL);
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
            //ESP_LOGI(BLE_HOST_TAG, ESP_BD_ADDR_STR " INPUT: %8s, MAP: %2u, ID: %3u, Len: %d, Data:", 
            //ESP_BD_ADDR_HEX(bda), esp_hid_usage_str(param->input.usage), param->input.map_index, 
            //param->input.report_id, param->input.length);
            
            if (!strncmp(esp_hid_usage_str(param->input.usage), "KEYBOARD", 8)) {
                handle_input(param->input.data, param->input.length);
                //ESP_LOG_BUFFER_HEX(BLE_HOST_TAG, param->input.data, param->input.length);
            }
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
        xTaskCreate(&ble_task, "ble_scan_retry", BLE_TASK_SIZE, NULL, BLE_TASK_PRI, NULL);
        break;
    }

    default:
        break;
    }
}

// ble_task
// This is mostly used to setup the BLE scanning mechanism, and bonding mechanism, and re-opening
// bonded BLE devices once they're set up
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