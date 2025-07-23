// UART modem config

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lwip/netif.h"

extern struct netif ppp_netif;

// Set the task size of the modem emulator
#define MODEM_TASK_SIZE 8192

// Set the task priority for the modem emulator
#define MODEM_TASK_PRI 10

// Set the task size for the custom DNS server
#define DNS_TASK_SIZE 8192

// Set the task priority for the custom DNS server that
// allows captive portaling as well as regular DNS requests to
// be serviced
#define DNS_TASK_PRI 8

// Set the task size for the built in HTTP server that
// provides captive portal services for activation and
// home page service as well as configuration services
#define HTTP_UI_TASK_SIZE 8192

// Set the priority for the build in HTTP_UI server
#define HTTP_UI_TASK_PRI 8

// Pin Mapping:
// SIGNAL     |GAL_BIT |GAL_PIN  |ESP32_GPIO
// MODEM_RX   |2       |28       |20
// MODEM_TX   |3       |29       |19
// MODEM_RTS  |4       |30       |47  
// MODEM_CTS  |5       |31       |21  
// GND        |GND     |26       |GND

// ESP32 <-> GPIO modem Pinouts
#define MODEM_RX  GPIO_NUM_20
#define MODEM_TX  GPIO_NUM_19
#define MODEM_RTS GPIO_NUM_47
#define MODEM_CTS GPIO_NUM_21
#define MODEM_UART UART_NUM_1

// ESP32 UART port to use for modem
#define UART_PORT UART_NUM_1

// Buffer size to use for UART modem
#define UART_BUFSIZE 512

// Timeout for UART modem
#define UART_TIMEOUT 50

// prototypes
void init_uart(void);
void modem_task(void *arg);

#ifdef __cplusplus
}
#endif
