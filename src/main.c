#include <math.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "bme280.h"
#include "bt_central.h"
#include "epd.h"
#include "fonts.h"
#include "icons.h"
#include "sensor.h"

#define EPD_REFRESH_TIME_MS 30000
#define SAMPLING_INTERVAL_MS 15000
#define PRESSURE_OFFSET 37

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

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

#define REMOTE_VALUES_ARE_READY() (remote_values_ready_mask == 0x0F)

static int local_battery_voltage_sample_once()
{
    int err;
    uint16_t battery_millivolt;
    uint8_t battery_level;

    err = battery_get_millivolt(&battery_millivolt);
    if (err) {
        LOG_ERR("Failed to get battery millivolts (err %d)", err);
        return err;
    }
    err = battery_get_percentage(&battery_level, battery_millivolt);
    if (err) {
        LOG_ERR("Failed to calculate battery percentage (err %d)", err);
        return err;
    }

    local_battery_level = battery_level;
    local_battery_last_millivolt = battery_millivolt;

    return 0;
}

static void local_battery_voltage_sample_callback(uint16_t millivolt)
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
        LOG_ERR("Failed to calculate battery percentage (err %d)", err);
        return;
    }
    local_battery_level = percentage;

    LOG_INF("Battery is at %d mV (capacity %d%%, %s)", millivolt, percentage,
            local_battery_is_charging ? "charging" : "discharging");
}

static void local_battery_charging_state_callback(bool connected)
{
    local_battery_is_charging = connected;
    LOG_INF("Charger %s", connected ? "connected" : "disconnected");
}

static void local_temperature_callback(int16_t value)
{
    local_temperature = value;
}

static void local_humidity_callback(int16_t value)
{
    local_humidity = value;
}

static void local_pressure_callback(int16_t value)
{
    local_pressure = value;
}

static void remote_battery_level_callback(uint8_t value)
{
    remote_battery_level = value;
    remote_values_ready_mask |= 0x01;
}

static void remote_temperature_callback(int16_t value)
{
    remote_temperature = value;
    remote_values_ready_mask |= 0x02;
}

static void remote_humidity_callback(int16_t value)
{
    remote_humidity = value;
    remote_values_ready_mask |= 0x04;
}

static void remote_pressure_callback(int16_t value)
{
    remote_pressure = value;
    remote_values_ready_mask |= 0x08;
}

static void remote_connection_callback(bool connected)
{
    if (!connected) {
        remote_values_ready_mask = 0;
    }
}

static void draw_static_icons(void)
{
    epd_draw_icon(&IconBattery, frame_buffer, 198, 10, EPD_COLOR_BLACK, true);
    epd_draw_icon(&IconTemperature, frame_buffer, 198, 80, EPD_COLOR_BLACK, true);
    epd_draw_icon(&IconHumidity, frame_buffer, 190, 150, EPD_COLOR_BLACK, true);
    epd_draw_icon(&IconPressure, frame_buffer, 260, 220, EPD_COLOR_BLACK, true);
}

static void draw_connection_icon(void)
{
    epd_draw_icon(&IconConnection, frame_buffer, 10, 22, EPD_COLOR_BLACK, true);
}

static void draw_charging_icon(void)
{
    epd_draw_icon(&IconCharge, frame_buffer, 358, 22, EPD_COLOR_BLACK, true);
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

int initialize(void)
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

    err = bme280_init();
    if (err) {
        LOG_ERR("Failed to initialize bme280 device (error %d)", err);
        return -ENXIO;
    }

    err = battery_init();
    if (err) {
        LOG_ERR("Failed to initialize battery management (error %d)", err);
        return err;
    }

    err = battery_register_charging_callback(local_battery_charging_state_callback);
    if (err) {
        LOG_ERR("Failed to register charging callback (error %d)", err);
        return err;
    }

    err = battery_register_sample_callback(local_battery_voltage_sample_callback);
    if (err) {
        LOG_ERR("Failed to register sample callback (error %d)", err);
        return err;
    }

    err = battery_set_fast_charge();
    if (err) {
        LOG_ERR("Failed to set battery fast charging (error %d)", err);
        return err;
    }

    bme280_register_temperature_callback(local_temperature_callback);
    bme280_register_humidity_callback(local_humidity_callback);
    bme280_register_pressure_callback(local_pressure_callback);
    err = bme280_sample_once();
    if (err) {
        LOG_ERR("Failed to read bme280 sensor data (error %d)", err);
        return err;
    }
    bme280_start_sampling(SAMPLING_INTERVAL_MS);

    local_battery_is_charging = battery_is_charging();
    err = local_battery_voltage_sample_once();
    if (err) {
        LOG_ERR("Failed to sample battery voltage (error %d)", err);
        return err;
    }
    battery_start_sampling(SAMPLING_INTERVAL_MS);

    bt_central_register_battery_level_callback(remote_battery_level_callback);
    bt_central_register_temperature_callback(remote_temperature_callback);
    bt_central_register_humidity_callback(remote_humidity_callback);
    bt_central_register_pressure_callback(remote_pressure_callback);
    bt_central_register_connection_callback(remote_connection_callback);
    bt_central_start_scan();

    return 0;
}

int main(void)
{
    int err;
    int epd_refresh_timeout_ms = 0;
    bool connected = false;
    bool charging = false;
    bool can_display_remote_values = false;
    bool remote_values_are_ready = false;

    err = initialize();
    if (err) {
        return err;
    }

    while (1) {
        remote_values_are_ready = REMOTE_VALUES_ARE_READY();
        if (remote_values_are_ready) {
            can_display_remote_values = true;
        }
        if (connected != remote_values_are_ready) {
            connected = remote_values_are_ready;
            epd_refresh_timeout_ms = 0;
        }
        if (charging != local_battery_is_charging) {
            charging = local_battery_is_charging;
            epd_refresh_timeout_ms = 0;
        }

        if (epd_refresh_timeout_ms == 0) {
            if (epd_init()) {
                LOG_ERR("Unable to initialize e-Paper display");
                return -EIO;
            }
            epd_fill(frame_buffer, EPD_COLOR_WHITE);
            if (can_display_remote_values) {
                draw_remote_values();
            }
            draw_local_values();
            draw_static_icons();
            if (!connected) {
                draw_connection_icon();
            }
            if (charging) {
                draw_charging_icon();
            }
            epd_display(frame_buffer);
            epd_sleep();
            epd_refresh_timeout_ms = EPD_REFRESH_TIME_MS;
        }

        if (!connected) {
            for (int i = 0; i < 4; i++) {
                gpio_pin_toggle_dt(&led0);
                k_msleep(500);
            }
        } else {
            k_msleep(2000);
        }
        epd_refresh_timeout_ms -= 2000;
    }

    return 0;
}
