#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/netif.h"

extern struct netif ppp_netif;

// Pin Mapping:
// SIGNAL     |GAL_BIT |GAL_PIN  |ESP32_GPIO
// MODEM_RX   |2       |28       |20
// MODEM_TX   |3       |29       |19
// MODEM_RTS  |4       |30       |47  
// MODEM_CTS  |5       |31       |21  
// GND        |GND     |26       |GND

// Modem Pinouts
#define MODEM_RX  GPIO_NUM_20
#define MODEM_TX  GPIO_NUM_19
#define MODEM_RTS GPIO_NUM_47
#define MODEM_CTS GPIO_NUM_21
#define MODEM_UART UART_NUM_1

// UART config
#define UART_PORT UART_NUM_1

#define UART_BUFSIZE 512

#define UART_TIMEOUT 50


void init_uart(void);
void modem_task(void *arg);

#ifdef __cplusplus
}
#endif
