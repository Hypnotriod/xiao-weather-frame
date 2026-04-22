#ifndef __BT_CENTRAL_H__
#define __BT_CENTRAL_H__

#include <stdbool.h>
#include <stdint.h>

int bt_central_start_scan(void);

typedef void (*device_battery_level_callback_t)(uint8_t value);
typedef void (*device_temperature_callback_t)(int16_t value);
typedef void (*device_humidity_callback_t)(int16_t value);
typedef void (*device_pressure_callback_t)(int16_t value);
typedef void (*device_connection_callback_t)(bool connected);

void bt_central_register_battery_level_callback(device_battery_level_callback_t callback);
void bt_central_register_temperature_callback(device_temperature_callback_t callback);
void bt_central_register_humidity_callback(device_humidity_callback_t callback);
void bt_central_register_pressure_callback(device_pressure_callback_t callback);
void bt_central_register_connection_callback(device_connection_callback_t callback);

#endif  //__BT_CENTRAL_H__
