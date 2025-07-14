// TODO: Cleanup and add more support for Make/Break scancodes, and shift codes,
// and potentially just abandon terminal input all together at some point
// and just start working on bluetooth keyboard input

#include <ctype.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"
#include <ctype.h>
#include "keyboard.h"

#define HOME_SCANCODE    0x6C
#define END_SCANCODE     0x69
#define INSERT_SCANCODE  0x70
#define DELETE_SCANCODE  0x71
#define PGUP_SCANCODE    0x7D
#define PGDN_SCANCODE    0x7A
#define F1_SCANCODE      0x05
#define F2_SCANCODE      0x06
#define F3_SCANCODE      0x04
#define F4_SCANCODE      0x0C
#define F5_SCANCODE      0x03
#define F6_SCANCODE      0x0B
#define F7_SCANCODE      0x83
#define F8_SCANCODE      0x0A
#define F9_SCANCODE      0x01
#define F10_SCANCODE     0x09
#define F11_SCANCODE     0x78
#define F12_SCANCODE     0x07

#define KB_TIMEOUT 5

static const char *KEYBOARD_TAG = "KEYBOARD";

volatile uint8_t kb_buffer[KB_BUFFER_SIZE];
volatile uint8_t kb_state = 0;
volatile uint8_t kb_scancode = 0;
volatile uint8_t kb_bit_index = 0;
volatile uint8_t kb_head = 0;
volatile uint8_t kb_tail = 0;

static const uint8_t ascii_to_scancode[128] = {
    // Lowercase letters
    ['a'] = 0x1C, ['b'] = 0x32, ['c'] = 0x21, ['d'] = 0x23,
    ['e'] = 0x24, ['f'] = 0x2B, ['g'] = 0x34, ['h'] = 0x33,
    ['i'] = 0x43, ['j'] = 0x3B, ['k'] = 0x42, ['l'] = 0x4B,
    ['m'] = 0x3A, ['n'] = 0x31, ['o'] = 0x44, ['p'] = 0x4D,
    ['q'] = 0x15, ['r'] = 0x2D, ['s'] = 0x1B, ['t'] = 0x2C,
    ['u'] = 0x3C, ['v'] = 0x2A, ['w'] = 0x1D, ['x'] = 0x22,
    ['y'] = 0x35, ['z'] = 0x1A,

    // Digits
    ['1'] = 0x16, ['2'] = 0x1E, ['3'] = 0x26, ['4'] = 0x25,
    ['5'] = 0x2E, ['6'] = 0x36, ['7'] = 0x3D, ['8'] = 0x3E,
    ['9'] = 0x46, ['0'] = 0x45,

    // Space, and some common symbols
    [' '] = 0x29, ['-'] = 0x4E, ['='] = 0x55,
    ['['] = 0x54, [']'] = 0x5B, ['\\'] = 0x5D,
    [';'] = 0x4C, ['\'']= 0x52, ['`'] = 0x0E,
    [','] = 0x41, ['.'] = 0x49, ['/'] = 0x4A,

    // Control keys
    [0x1B] = 0x76, // ESC
    [0x0D] = 0x5A, // Enter
    [0x09] = 0x0D, // Tab
    [0x7F] = 0x66, // Backspace

};

// queue_scancode
// This is used to deal with KB input buffering. Because Sharkwire sometimes lags behind
// when it gets busy, we need to ensure 100% that any key that's pressed actually gets processed
// even if delayed. This simply fills the keyboard buffer which will be handled accordingly in
// our clk_isr_handler
void queue_scancode(uint8_t scancode) {
    uint8_t next_head = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next_head == kb_tail) {
        ESP_LOGW(KEYBOARD_TAG, "KB buffer overflow, dropping scancode");
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

// keyboard_task
// This handles all ESP32 to N64 keyboard functionality by setting up GPIO pin configuration
// and then contantly reading input and passing scancodes
void keyboard_task(void *arg) {
    ESP_LOGI(KEYBOARD_TAG, "keyboard_task started on core %d", xPortGetCoreID());

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);

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

    uint8_t ch;
    while (1) {
    // Read input over UART terminal connection
    int len = uart_read_bytes(UART_NUM_0, &ch, 1, 10 / portTICK_PERIOD_MS);
    if (len > 0 && ch < 128) {
        //printf("Received keyboard: 0x%02X\n", ch);

        if (ch == 0x1B) {
            uint8_t next;
            if (uart_read_bytes(UART_NUM_0, &next, 1, 10 / portTICK_PERIOD_MS) > 0) {
                if (next == 0x5B) {
                    if (uart_read_bytes(UART_NUM_0, &next, 1, 10 / portTICK_PERIOD_MS) > 0) {
                        switch (next) {
                            // Lazily handle arrow key input
                            case 0x41: queue_scancode(0x75); break; // Up
                            case 0x42: queue_scancode(0x72); break; // Down
                            case 0x43: queue_scancode(0x74); break; // Right
                            case 0x44: queue_scancode(0x6B); break; // Left
                            default: printf("UNHANDLED CSI %02X\n", next); break;
                        }
                    }
                } else {
                    printf("UNHANDLED ESC %02X\n", next);
                }
            }
        } else {
            // Translate input to PS/2 set2 scancode
            uint8_t sc = ascii_to_scancode[ch];
            if (sc) {
                // Send the MAKE scancode for input
                queue_scancode(sc);
            } else {
                printf("No scancode mapped for %c\n", ch);
            }
        }

        taskYIELD();
        }
    }
}