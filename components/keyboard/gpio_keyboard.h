#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SCAN_DURATION_SECONDS 10

#include "esp_event.h"

#define MAX_KEYS 6
#define KB_BUFFER_SIZE 64
#define MCU_TO_N64_KB_CLK   48
#define MCU_TO_N64_KB_DATA  45

void ble_task(void *arg);
void init_gpio_keyboard(void);
void queue_scancode(uint8_t scancode);
char *bda2str(uint8_t *bda, char *str, size_t size);
void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);

#ifdef __cplusplus
}
#endif
