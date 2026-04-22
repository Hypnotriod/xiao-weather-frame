

#include "bme280.h"

#include <math.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor_data_types.h>
#include <zephyr/logging/log.h>

#include "sensor.h"

LOG_MODULE_REGISTER(bm280, LOG_LEVEL_INF);

static const struct device* const bme280_dev = DEVICE_DT_GET(DT_NODELABEL(bme280_dev));
SENSOR_DT_READ_IODEV(bme280_iodev, DT_NODELABEL(bme280_dev), {SENSOR_CHAN_AMBIENT_TEMP, 0}, {SENSOR_CHAN_HUMIDITY, 0},
                     {SENSOR_CHAN_PRESS, 0});
RTIO_DEFINE(bme280_ctx, 1, 1);
static const struct sensor_decoder_api* decoder;
static struct k_work_delayable sample_periodic_work;

static uint32_t sampling_interval_ms;

static bme280_temperature_callback_t temperature_callback = NULL;
static bme280_humidity_callback_t humidity_callback = NULL;
static bme280_pressure_callback_t pressure_callback = NULL;

static int16_t sensor_q31_data_to_int16_attr(struct sensor_q31_data* data, uint8_t pow)
{
    float32_t v = data->readings[0].value;
    int8_t s = data->shift;

    return v / (1 << (31 - s)) * powf(10.0f, (float32_t)pow);
}

int bme280_sample_once()
{
    uint8_t buff[32];
    uint32_t temp_fit = 0;
    uint32_t press_fit = 0;
    uint32_t hum_fit = 0;
    struct sensor_q31_data temp_data = {0};
    struct sensor_q31_data press_data = {0};
    struct sensor_q31_data hum_data = {0};
    int16_t temperature = 0;
    int16_t pressure = 0;
    int16_t humidity = 0;
    int err;

    err = sensor_read(&bme280_iodev, &bme280_ctx, buff, sizeof(buff));
    if (err) {
        LOG_ERR("Failed to read sensor data (error %d)", err);
        return err;
    }

    err = sensor_get_decoder(bme280_dev, &decoder);
    if (err) {
        LOG_ERR("Failed to get sensor's decoder API (error %d)", err);
        return err;
    }

    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_AMBIENT_TEMP, 0}, &temp_fit, 1, &temp_data);
    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_PRESS, 0}, &press_fit, 1, &press_data);
    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_HUMIDITY, 0}, &hum_fit, 1, &hum_data);

    temperature = sensor_q31_data_to_int16_attr(&temp_data, 2);
    pressure = sensor_q31_data_to_int16_attr(&press_data, 1);
    humidity = sensor_q31_data_to_int16_attr(&hum_data, 2);

    if (temperature_callback != NULL) {
        temperature_callback(temperature);
    }
    if (pressure_callback != NULL) {
        pressure_callback(pressure);
    }
    if (humidity_callback != NULL) {
        humidity_callback(humidity);
    }

    LOG_INF("Temp: %i.%02i DegC; Press: %i hPa; Humidity: %i.%02i %%RH", SENSOR_VAL_FORMAT(temperature), pressure,
            SENSOR_VAL_FORMAT(humidity));

    return 0;
}

static void bm280_sample_periodic_handler(struct k_work* work)
{
    bme280_sample_once();
    k_work_reschedule(&sample_periodic_work, K_MSEC(sampling_interval_ms));
}

void bme280_start_sampling(uint32_t interval_ms)
{
    sampling_interval_ms = interval_ms;
    k_work_init_delayable(&sample_periodic_work, bm280_sample_periodic_handler);
    k_work_schedule(&sample_periodic_work, K_MSEC(sampling_interval_ms));
}

void bme280_register_temperature_callback(bme280_temperature_callback_t callback)
{
    temperature_callback = callback;
}

void bme280_register_humidity_callback(bme280_humidity_callback_t callback)
{
    humidity_callback = callback;
}

void bme280_register_pressure_callback(bme280_pressure_callback_t callback)
{
    pressure_callback = callback;
}

int bme280_init()
{
    if (bme280_dev == NULL) {
        LOG_ERR("No bme280 device found");
        return -ENXIO;
    }

    if (!device_is_ready(bme280_dev)) {
        LOG_ERR("bme280 device is not ready");
        return -EIO;
    }

    return 0;
}
