#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "battery.h"
#include "bt_central.h"

#define SAMPLING_INTERVAL_MS 5000

static volatile bool is_charging = false;

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void battery_voltage_update(uint16_t millivolt)
{
    uint8_t percentage = 0;

    int err = battery_get_percentage(&percentage, millivolt);
    if (err) {
        LOG_ERR("Failed to calculate battery percentage");
        return;
    }

    LOG_INF("Battery is at %d mV (capacity %d%%, %s)", millivolt, percentage, is_charging ? "charging" : "discharging");
}

static void battery_charging_state_update(bool connected)
{
    is_charging = connected;
    LOG_INF("Charger %s", connected ? "connected" : "disconnected");
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

    err = battery_register_charging_callback(battery_charging_state_update);
    if (err) {
        LOG_ERR("Failed to register charging callback (error %d)", err);
        return err;
    }

    err = battery_register_sample_callback(battery_voltage_update);
    if (err) {
        LOG_ERR("Failed to register sample callback (error %d)", err);
        return err;
    }

    err = battery_set_fast_charge();
    if (err) {
        LOG_ERR("Failed to set battery fast charging (error %d)", err);
        return err;
    }

    battery_start_sampling(SAMPLING_INTERVAL_MS);
    bt_central_start_scan();

    return 0;
}
