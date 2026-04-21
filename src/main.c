#include <math.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor_data_types.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "bt_central.h"
#include "epd.h"
#include "fonts.h"
#include "icons.h"

#define EPD_REFRESH_TIME_MS 30000
#define SAMPLING_INTERVAL_MS (EPD_REFRESH_TIME_MS / 2)
#define PRESSURE_OFFSET 37

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

const struct device* const bme280_dev = DEVICE_DT_GET(DT_NODELABEL(bme280_dev));
SENSOR_DT_READ_IODEV(bme280_iodev, DT_NODELABEL(bme280_dev), {SENSOR_CHAN_AMBIENT_TEMP, 0}, {SENSOR_CHAN_HUMIDITY, 0},
                     {SENSOR_CHAN_PRESS, 0});
RTIO_DEFINE(bme280_ctx, 1, 1);
static const struct sensor_decoder_api* decoder;
static struct k_work_delayable sample_periodic_work;

static uint8_t frame_buffer[EPD_FRAME_BUFFER_SIZE] = {0};

static volatile bool local_battery_is_charging = false;
static volatile uint16_t local_battery_last_millivolt = 0;
static volatile uint8_t local_battery_level = 0;
static volatile int16_t local_temperature = 0;
static volatile int16_t local_pressure = 0;
static volatile int16_t local_humidity = 0;

static volatile uint8_t remote_values_ready_mask = 0;
static volatile uint8_t remote_battery_level;
static volatile int16_t remote_temperature;
static volatile int16_t remote_humidity;
static volatile int16_t remote_pressure;

#define REMOTE_VALUES_ARE_NOT_READY() remote_values_ready_mask != 0x0F

static int16_t sensor_q31_data_to_int16_attr(struct sensor_q31_data* data, uint8_t pow)
{
    float32_t v = data->readings[0].value;
    int8_t s = data->shift;

    return v / (1 << (31 - s)) * powf(10.0f, (float32_t)pow);
}

static int bm3280_sample()
{
    uint8_t buff[32];
    uint32_t temp_fit = 0;
    uint32_t press_fit = 0;
    uint32_t hum_fit = 0;
    struct sensor_q31_data temp_data = {0};
    struct sensor_q31_data press_data = {0};
    struct sensor_q31_data hum_data = {0};
    int err;

    err = sensor_read(&bme280_iodev, &bme280_ctx, buff, sizeof(buff));
    if (err) {
        LOG_ERR("Failed to read the sensor data (error %d)", err);
        return err;
    }

    err = sensor_get_decoder(bme280_dev, &decoder);
    if (err) {
        LOG_ERR("Failed to get the sensor's decoder API (error %d)", err);
        return err;
    }

    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_AMBIENT_TEMP, 0}, &temp_fit, 1, &temp_data);
    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_PRESS, 0}, &press_fit, 1, &press_data);
    decoder->decode(buff, (struct sensor_chan_spec){SENSOR_CHAN_HUMIDITY, 0}, &hum_fit, 1, &hum_data);

    local_temperature = sensor_q31_data_to_int16_attr(&temp_data, 2);
    local_pressure = sensor_q31_data_to_int16_attr(&press_data, 1);
    local_humidity = sensor_q31_data_to_int16_attr(&hum_data, 2);

    LOG_INF("Temp: %i.%02i DegC; Press: %i hPa; Humidity: %i.%02i %%RH", SENSOR_VAL_FORMAT(local_temperature),
            local_pressure, SENSOR_VAL_FORMAT(local_humidity));

    return 0;
}

static void bm3280_sample_periodic_handler(struct k_work* work)
{
    bm3280_sample();
    k_work_reschedule(&sample_periodic_work, K_MSEC(SAMPLING_INTERVAL_MS));
}

static void battery_voltage_read_once()
{
    uint16_t battery_millivolt;
    uint8_t battery_level;

    battery_get_millivolt(&battery_millivolt);
    battery_get_percentage(&battery_level, battery_millivolt);

    local_battery_level = battery_level;
    local_battery_last_millivolt = battery_millivolt;
}

static void battery_voltage_sample_callback(uint16_t millivolt)
{
    uint8_t percentage = 0;

    if (local_battery_is_charging && millivolt < local_battery_last_millivolt) {
        millivolt = local_battery_last_millivolt;
    } else if (!local_battery_is_charging && millivolt > local_battery_last_millivolt) {
        millivolt = local_battery_last_millivolt;
    }
    local_battery_last_millivolt = millivolt;

    int err = battery_get_percentage(&percentage, millivolt);
    if (err) {
        LOG_ERR("Failed to calculate battery percentage");
        return;
    }
    local_battery_level = percentage;

    LOG_INF("Battery is at %d mV (capacity %d%%, %s)", millivolt, percentage,
            local_battery_is_charging ? "charging" : "discharging");
}

static void battery_charging_state_callback(bool connected)
{
    local_battery_is_charging = connected;
    LOG_INF("Charger %s", connected ? "connected" : "disconnected");
}

static void battery_level_callback(uint8_t value)
{
    remote_battery_level = value;
    remote_values_ready_mask |= 0x01;
}

static void temperature_callback(int16_t value)
{
    remote_temperature = value;
    remote_values_ready_mask |= 0x02;
}

static void humidity_callback(int16_t value)
{
    remote_humidity = value;
    remote_values_ready_mask |= 0x04;
}

static void pressure_callback(int16_t value)
{
    remote_pressure = value;
    remote_values_ready_mask |= 0x08;
}

static void connection_callback(bool connected)
{
    if (!connected) {
        remote_values_ready_mask = 0;
    }
}

static void draw_icons(void)
{
    epd_draw_icon(&IconBattery, frame_buffer, 198, 10, EPD_COLOR_BLACK, false);
    epd_draw_icon(&IconTemperature, frame_buffer, 198, 80, EPD_COLOR_BLACK, false);
    epd_draw_icon(&IconHumidity, frame_buffer, 190, 150, EPD_COLOR_BLACK, false);
    epd_draw_icon(&IconPressure, frame_buffer, 260, 220, EPD_COLOR_BLACK, false);
}

static void draw_remote_values(void)
{
    char buff[16];
    uint8_t batt = remote_battery_level;
    int16_t temp = remote_temperature;
    int16_t hum = remote_humidity;
    int16_t press = remote_pressure;

    sprintf(buff, "%6u%%", batt);
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 10, 20, EPD_COLOR_BLACK, false);

    sprintf(buff, "%+3i.%02iC", SENSOR_VAL_FORMAT(temp));
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 10, 90, EPD_COLOR_BLACK, false);

    sprintf(buff, "%3i.%02i%%", SENSOR_VAL_FORMAT(hum));
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 10, 160, EPD_COLOR_BLACK, false);

    sprintf(buff, "%4ihPa", press + PRESSURE_OFFSET);
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 70, 230, EPD_COLOR_BLACK, false);
}

static void draw_local_values(void)
{
    char buff[16];
    uint8_t batt = local_battery_level;
    int16_t temp = local_temperature;
    int16_t hum = local_humidity;

    sprintf(buff, "%u%%", batt);
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 242, 20, EPD_COLOR_BLACK, false);

    sprintf(buff, "%+i.%iC", SENSOR_VAL_FORMAT_SHORT(temp));
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 242, 90, EPD_COLOR_BLACK, false);

    sprintf(buff, "%i.%i%%", SENSOR_VAL_FORMAT_SHORT(hum));
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 242, 160, EPD_COLOR_BLACK, false);
}

int main(void)
{
    int err;

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Unable to initialize Bluetooth (err %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("LED 0 pin configure failed (err %d)", err);
        return err;
    }

    if (bme280_dev == NULL) {
        LOG_ERR("No BME280 device found");
        return -ENXIO;
    }

    if (!device_is_ready(bme280_dev)) {
        LOG_ERR("BME280 device is not ready");
        return -EIO;
    }

    err = battery_init();
    if (err) {
        LOG_ERR("Failed to initialize battery management (error %d)", err);
        return err;
    }

    err = battery_register_charging_callback(battery_charging_state_callback);
    if (err) {
        LOG_ERR("Failed to register charging callback (error %d)", err);
        return err;
    }

    err = battery_register_sample_callback(battery_voltage_sample_callback);
    if (err) {
        LOG_ERR("Failed to register sample callback (error %d)", err);
        return err;
    }

    err = battery_set_fast_charge();
    if (err) {
        LOG_ERR("Failed to set battery fast charging (error %d)", err);
        return err;
    }

    if (epd_init()) {
        LOG_ERR("Unable to initialize e-Paper display");
        return -EIO;
    }

    bm3280_sample();

    k_work_init_delayable(&sample_periodic_work, bm3280_sample_periodic_handler);
    k_work_schedule(&sample_periodic_work, K_MSEC(SAMPLING_INTERVAL_MS));

    bt_central_register_battery_level_callback(battery_level_callback);
    bt_central_register_temperature_callback(temperature_callback);
    bt_central_register_humidity_callback(humidity_callback);
    bt_central_register_pressure_callback(pressure_callback);
    bt_central_register_connection_callback(connection_callback);
    bt_central_start_scan();

    local_battery_is_charging = battery_is_charging();
    battery_voltage_read_once();
    battery_start_sampling(SAMPLING_INTERVAL_MS);

    epd_fill(frame_buffer, EPD_COLOR_WHITE);

    while (1) {
        if (REMOTE_VALUES_ARE_NOT_READY()) {
            epd_init();
            epd_draw_string("Connecting...", &Font16, frame_buffer, 5, 5, EPD_COLOR_BLACK, false);
            draw_icons();
            draw_local_values();
            epd_display(frame_buffer);
            epd_sleep();
            while (REMOTE_VALUES_ARE_NOT_READY()) {
                gpio_pin_set(led0.port, led0.pin, 1);
                k_msleep(500);
                gpio_pin_set(led0.port, led0.pin, 0);
                k_msleep(500);
            }
        }

        epd_init();
        epd_fill(frame_buffer, EPD_COLOR_WHITE);
        draw_remote_values();
        draw_icons();
        draw_local_values();
        epd_display(frame_buffer);
        epd_sleep();
        k_msleep(EPD_REFRESH_TIME_MS);
    }

    return 0;
}
