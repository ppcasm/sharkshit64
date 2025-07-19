#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "modem.h"
#include "gpio_keyboard.h"

static const char *MAIN_TAG = "MAIN";

void app_main(void) {

    ESP_LOGI(MAIN_TAG, "app_main started on core %d", xPortGetCoreID());

    esp_err_t ret;
    #if HID_HOST_MODE == HIDH_IDLE_MODE
        ESP_LOGE(MAIN_TAG, "Please turn on BT HID host or BLE!");
        return;
    #endif

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK( ret );
    ESP_LOGI(MAIN_TAG, "setting hid gap, mode:%d", HID_HOST_MODE);
    ESP_ERROR_CHECK( esp_hid_gap_init(HID_HOST_MODE) );
    ESP_ERROR_CHECK( esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler) );

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK( esp_hidh_init(&config) );

    char bda_str[18] = {0};
    ESP_LOGI(MAIN_TAG, "Own address:[%s]", bda2str((uint8_t *)esp_bt_dev_get_address(), bda_str, sizeof(bda_str)));

    // Start the BLE side of the keyboard interface
    xTaskCreate(&ble_task, "ble_task", 8192, NULL, 5, NULL);

    // Start the UART modem interface task
    xTaskCreatePinnedToCore(modem_task, "modem_task", 8192, NULL, 10, NULL, 1);
}
