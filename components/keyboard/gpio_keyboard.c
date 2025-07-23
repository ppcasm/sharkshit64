#include <ctype.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"
#include "keyboard.h"

static const char *GPIO_KEYBOARD_TAG = "GPIO_KEYBOARD";

volatile uint8_t kb_buffer[KB_BUFFER_SIZE];
volatile uint8_t kb_state = 0;
volatile uint8_t kb_scancode = 0;
volatile uint8_t kb_bit_index = 0;
volatile uint8_t kb_head = 0;
volatile uint8_t kb_tail = 0;

// queue_scancode
// This is used to deal with KB input buffering. Because Sharkwire sometimes lags behind
// when it gets busy, we need to ensure 100% that any key that's pressed actually gets processed
// even if delayed. This simply fills the keyboard buffer which will be handled accordingly in
// our clk_isr_handler
void queue_scancode(uint8_t scancode) {
    uint8_t next_head = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next_head == kb_tail) {
        ESP_LOGW(GPIO_KEYBOARD_TAG, "KB buffer overflow, dropping scancode");
        return;
    }

    kb_buffer[kb_head] = scancode;
    kb_head = next_head;

    // If we're idle, kickstart the transmission immediately
    if (kb_state == 0) {
        kb_scancode = kb_buffer[kb_tail];
        kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
        gpio_set_level(MCU_TO_N64_KB_DATA, 1); // SYNC bit
        kb_state = 1;
    }
}

// clk_isr_handler
// We store the actual handler for the KB protocol in IRAM to make it faster
// and more robust. This works by setting up the protocol in stages based on kb_state
// which gives us more flexibility over dealing with any desync or lag, as well as allowing
// us to externally monitor what stage we are currently processing scancodes at just in
// case we get a scancode that's taking too long to process
//
// Basically the protocol is that DATA goes high and it triggers the N64 KB event loop to
// make the N64 expect that data is coming (stage 0, which is SYNC and works based off queued
// input from our KB buffer) and then the N64 will start driving the clock. It's at
// that point that we want to send a start bit (0) on the first clock (stage 1), then move
// to the next stage, which is to send the 8-bit scancode (LSB first) on clock cycle 2~9 (stage 2) 
// and then move to the next stage (stage 3) which will clock in the stop bit (DATA = 1) and 
// move to the next stage (stage 4) which is to simply return to IDLE (DATA = 0) at which point
// it will check if there's any buffered KB scancodes and process those in the same way by setting
// the SYNC bit (DATA = 1) and doing another transaction
//
// All signals are FALLING edge triggered, and the CLK signal is set up to run this function every
// time CLK goes low which cuts down on CPU waste by not having to poll.
void IRAM_ATTR clk_isr_handler(void* arg) {
    switch (kb_state) {
        case 1: // Send start bit
            gpio_set_level(MCU_TO_N64_KB_DATA, 0);
            kb_state = 2;
            kb_bit_index = 0;
            break;

        case 2: // Send scancode bits LSB first
            gpio_set_level(MCU_TO_N64_KB_DATA, (kb_scancode >> kb_bit_index) & 1);
            kb_bit_index++;
            if (kb_bit_index >= 8) {
                kb_state = 3;
            }
            break;

        case 3: // Send stop bit
            gpio_set_level(MCU_TO_N64_KB_DATA, 1);
            kb_state = 4;
            break;

        case 4: // Return to idle (DATA=0)
            gpio_set_level(MCU_TO_N64_KB_DATA, 0);
            kb_state = 0;

            // Check if more scancodes queued
            if (kb_head != kb_tail) {
                kb_scancode = kb_buffer[kb_tail];
                kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;

                // Immediately start new transmission
                gpio_set_level(MCU_TO_N64_KB_DATA, 1);
                kb_state = 1;
            }
            break;

        default:
            break;
    }
}

// init_gpio_keyboard
// This sets up the GPIO pin configuration for the GPIO side of the sharkwire keyboard interface
void init_gpio_keyboard() {
    ESP_LOGI(GPIO_KEYBOARD_TAG, "GPIO_Keyboard Initialized");

    // DATA pin as output
    gpio_config_t data_conf = {
        .pin_bit_mask = (1ULL << MCU_TO_N64_KB_DATA),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&data_conf);

    // CLK pin as input with interrupt on FALLING edge
    gpio_config_t clk_conf = {
        .pin_bit_mask = (1ULL << MCU_TO_N64_KB_CLK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 1,
        .intr_type = GPIO_INTR_NEGEDGE
    };

    gpio_config(&clk_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(MCU_TO_N64_KB_CLK, clk_isr_handler, NULL);

    // Set initial DATA level (idle = low)
    gpio_set_level(MCU_TO_N64_KB_DATA, 0);

 }