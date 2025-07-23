// Keyboard config

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_event.h"

// Set the allocated task size for the keyboard_repeat_task
#define KBD_REPEAT_TASK_SIZE 8192

// Set the keyboard_repeat_task priority
#define KBD_REPEAT_TASK_PRI 5

// Set the allocated task size for the ble_task (used for handling BLE connection)
#define BLE_TASK_SIZE 8192

// Set the ble_task priority
#define BLE_TASK_PRI 5

// Set the duration that you want to scan for BLE devices during bonding process
#define BLE_SCAN_DURATION_SECONDS 10

// Set max keys allowed in the BLE HID input report
#define MAX_KEYS 6

// This value sets the basic time resolution of the keyboard repeat logic
// by defining how often the keyboard_tick() function is called, in milliseconds
//
// So KB_TICK_RATE = 10 means the repeat task runs every 10 ms
// and that comes to 100 times per second (1000ms ÷ 10ms = 100 Hz)
#define KB_TICK_RATE 10

// This is the number of keyboard_tick() calls to wait before the key starts repeating
// and so if the tick happens every 10 ms, the total delay is:
// REPEAT_DELAY_TICKS × KB_TICK_RATE
// = 50 × 10 ms
// = 500 ms
//
// So after holding a key for half a second, it starts to auto-repeat
#define REPEAT_DELAY_TICKS 50

// This defines how often a held key repeats after the initial delay
// So that goes like this:
// REPEAT_INTERVAL_TICKS × KB_TICK_RATE
// = 10 × 10 ms
// = 100 ms
//
// So once repeating starts, the key is sent again every 100ms (10 characters/second).
#define REPEAT_INTERVAL_TICKS 10  // ~100ms repeat rate

// Set the GPIO keyboard buffer size
// More is not always better, and the dequeue rate is strictly controlled by N64 since
// it controls the clock and has it's own keyboard buffer (~16 bytes)
#define KB_BUFFER_SIZE 32

// Obviously you define the pins you want to use for the MCU_TO_N64 UART Modem here
#define MCU_TO_N64_KB_CLK   48
#define MCU_TO_N64_KB_DATA  45

// Prototypes
void ble_task(void *arg);
void init_gpio_keyboard(void);
void queue_scancode(uint8_t scancode);
char *bda2str(uint8_t *bda, char *str, size_t size);
void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);

#ifdef __cplusplus
}
#endif
