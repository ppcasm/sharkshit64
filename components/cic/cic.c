#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *CIC_TAG = "CIC";

// Potentially use this to do a CIC emulator at some point
void cic_task() {
    ESP_LOGI(CIC_TAG, "cic_task started on core %d", xPortGetCoreID());
}