#pragma once

#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

bool is_ble_connected(void);
bool unbond_ble_device(void);
int get_ble_battery_level(void);
const char *get_ble_manufacturer(void);
bool get_bonded_device_address(esp_bd_addr_t out_bda);

#ifdef __cplusplus
}
#endif