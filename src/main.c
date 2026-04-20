#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "bt_central.h"
#include "epd.h"
#include "fonts.h"
#include "icons.h"

#define SAMPLING_INTERVAL_MS 5000
#define EPD_REFRESH_TIME_MS 30000
#define PRESSURE_OFFSET 37

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static uint8_t frame_buffer[EPD_FRAME_BUFFER_SIZE] = {0};

static volatile bool local_battery_is_charging = false;
static volatile uint8_t local_battery_level;

static volatile uint8_t remote_values_ready_mask = 0;
static volatile uint8_t remote_battery_level;
static volatile int16_t remote_temperature;
static volatile int16_t remote_humidity;
static volatile int16_t remote_pressure;

#define REMOTE_VALUES_ARE_NOT_READY() remote_values_ready_mask != 0x0F

static void battery_voltage_read_once()
{
    uint16_t battery_millivolt;
    uint8_t battery_level;

    battery_get_millivolt(&battery_millivolt);
    battery_get_percentage(&battery_level, battery_millivolt);

    local_battery_level = battery_level;
}

static void battery_voltage_sample_callback(uint16_t millivolt)
{
    uint8_t percentage = 0;

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
    char buff[10];
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
    char buff[10];
    uint8_t batt = local_battery_level;

    sprintf(buff, "%u%%", batt);
    epd_draw_string(buff, &FontRobotoBold40, frame_buffer, 242, 20, EPD_COLOR_BLACK, false);
    sprintf(buff, "%u%%", batt);
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
