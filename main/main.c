#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "modem.h"
#include "keyboard.h"

static const char *TAG = "MAIN";

void app_main(void) {

    ESP_LOGI(TAG, "app_main started on core %d", xPortGetCoreID());

    // Start the UART modem interface task
    xTaskCreatePinnedToCore(modem_task, "modem_task", 8192, NULL, 10, NULL, 1);

    // Start keyboard interface task
    xTaskCreatePinnedToCore(keyboard_task, "keyboard_task", 4096, NULL, 20, NULL, 0);
}
