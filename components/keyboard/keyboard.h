#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define KB_BUFFER_SIZE 64
#define MCU_TO_N64_KB_CLK   48
#define MCU_TO_N64_KB_DATA  45

void keyboard_task(void *arg);
void send_scancode(uint8_t scancode);

#ifdef __cplusplus
}
#endif
