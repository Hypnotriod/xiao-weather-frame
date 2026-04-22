#ifndef __BME280_H__
#define __BME280_H__

#include <stdint.h>

typedef void (*bme280_temperature_callback_t)(int16_t value);
typedef void (*bme280_humidity_callback_t)(int16_t value);
typedef void (*bme280_pressure_callback_t)(int16_t value);

int bme280_init();
void bme280_register_temperature_callback(bme280_temperature_callback_t callback);
void bme280_register_humidity_callback(bme280_humidity_callback_t callback);
void bme280_register_pressure_callback(bme280_pressure_callback_t callback);
int bme280_sample_once();
void bme280_start_sampling(uint32_t interval_ms);

#endif  //__BME280_H__