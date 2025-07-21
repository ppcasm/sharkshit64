#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_event.h"

#define BLE_SCAN_DURATION_SECONDS 10

// Set max keys allowed in the BLE HID input report
#define MAX_KEYS 6

// This value sets the basic time resolution of the keyboard repeat logic
// by defining how often the keyboard_tick() function is called, in milliseconds
//
// So KB_TICK_RATE = 20 means the repeat task runs every 20 ms
// and that comes to 50 times per second (1000ms ÷ 20ms = 50 Hz)
#define KB_TICK_RATE 20

// This is the number of keyboard_tick() calls to wait before the key starts repeating
// and so if the tick happens every 20 ms, the total delay is:
// REPEAT_DELAY_TICKS × KB_TICK_RATE
// = 25 × 20 ms
// = 500 ms
//
// So after holding a key for half a second, it starts to auto-repeat
#define REPEAT_DELAY_TICKS 25

// This defines how often a held key repeats after the initial delay
// So that goes like this:
// REPEAT_INTERVAL_TICKS × KB_TICK_RATE
// = 5 × 20 ms
// = 100 ms
//
// So once repeating starts, the key is sent again every 100ms (10 characters/second).
#define REPEAT_INTERVAL_TICKS 5   // ~100ms repeat rate

// We use a software keyboard buffer
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
