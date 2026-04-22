#include "bt_central.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "sensor.h"

LOG_MODULE_REGISTER(bt_central, LOG_LEVEL_INF);

static const struct bt_uuid* UUID_BAS = BT_UUID_BAS;
static const struct bt_uuid* UUID_ESS = BT_UUID_ESS;
static const struct bt_uuid* UUID_BAS_BATTERY_LEVEL = BT_UUID_BAS_BATTERY_LEVEL;
static const struct bt_uuid* UUID_TEMPERATURE = BT_UUID_TEMPERATURE;
static const struct bt_uuid* UUID_HUMIDITY = BT_UUID_HUMIDITY;
static const struct bt_uuid* UUID_PRESSURE = BT_UUID_PRESSURE;
static const struct bt_uuid* UUID_GATT_CCC = BT_UUID_GATT_CCC;

static int discover_step = 0;
static struct bt_conn* default_conn;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_battery_level_params;
static struct bt_gatt_subscribe_params subscribe_temperature_params;
static struct bt_gatt_subscribe_params subscribe_humidity_params;
static struct bt_gatt_subscribe_params subscribe_pressure_params;
static struct bt_gatt_read_params read_battery_level_params;
static struct bt_gatt_read_params read_temperature_params;
static struct bt_gatt_read_params read_humidity_params;
static struct bt_gatt_read_params read_pressure_params;

static device_battery_level_callback_t battery_level_callback = NULL;
static device_temperature_callback_t temperature_callback = NULL;
static device_humidity_callback_t humidity_callback = NULL;
static device_pressure_callback_t pressure_callback = NULL;
static device_connection_callback_t connection_callback = NULL;

static void discover_ess(void);
static uint8_t discover_ess_func(struct bt_conn* conn, const struct bt_gatt_attr* attr,
                                 struct bt_gatt_discover_params* params);
static void discover_bas(void);
static uint8_t discover_bas_func(struct bt_conn* conn, const struct bt_gatt_attr* attr,
                                 struct bt_gatt_discover_params* params);

static uint8_t read_battery_level_func(struct bt_conn* conn, uint8_t err, struct bt_gatt_read_params* params,
                                       const void* data, uint16_t length)
{
    if (err || !data || length != 1) {
        LOG_ERR("Read Battery Level failed (err %d)", err);
        params->single.handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    uint8_t battery_level = ((uint8_t*)data)[0];
    if (battery_level_callback != NULL) {
        battery_level_callback(battery_level);
    }

    LOG_INF("[READ] Battery Level %u%%", battery_level);

    return BT_GATT_ITER_STOP;
}

static uint8_t notify_battery_level_func(struct bt_conn* conn, struct bt_gatt_subscribe_params* params,
                                         const void* data, uint16_t length)
{
    if (!data || length != 1) {
        LOG_INF("[UNSUBSCRIBED] Battery Level");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    uint8_t battery_level = ((uint8_t*)data)[0];
    if (battery_level_callback != NULL) {
        battery_level_callback(battery_level);
    }

    LOG_INF("[NOTIFICATION] Battery Level %u%%", battery_level);

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_temperature_func(struct bt_conn* conn, uint8_t err, struct bt_gatt_read_params* params,
                                     const void* data, uint16_t length)
{
    if (err || !data || length != 2) {
        LOG_ERR("Read Temperature failed (err %d)", err);
        params->single.handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t temperature = ((int16_t*)data)[0];
    if (temperature_callback != NULL) {
        temperature_callback(temperature);
    }

    LOG_INF("[READ] Temperature %i.%02iC", SENSOR_VAL_FORMAT(temperature));

    return BT_GATT_ITER_STOP;
}

static uint8_t notify_temperature_func(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data,
                                       uint16_t length)
{
    if (!data || length != 2) {
        LOG_INF("[UNSUBSCRIBED] Temperature");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t temperature = ((int16_t*)data)[0];
    if (temperature_callback != NULL) {
        temperature_callback(temperature);
    }

    LOG_INF("[NOTIFICATION] Temperature %i.%02iC", SENSOR_VAL_FORMAT(temperature));

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_humidity_func(struct bt_conn* conn, uint8_t err, struct bt_gatt_read_params* params,
                                  const void* data, uint16_t length)
{
    if (err || !data || length != 2) {
        LOG_ERR("Read Humidity failed (err %d)", err);
        params->single.handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t humidity = ((int16_t*)data)[0];
    if (humidity_callback != NULL) {
        humidity_callback(humidity);
    }

    LOG_INF("[READ] Humidity: %i.%02i%%", SENSOR_VAL_FORMAT(humidity));

    return BT_GATT_ITER_STOP;
}

static uint8_t notify_humidity_func(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data,
                                    uint16_t length)
{
    if (!data || length != 2) {
        LOG_INF("[UNSUBSCRIBED] Humidity");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t humidity = ((int16_t*)data)[0];
    if (humidity_callback != NULL) {
        humidity_callback(humidity);
    }

    LOG_INF("[NOTIFICATION] Humidity: %i.%02i%%", SENSOR_VAL_FORMAT(humidity));

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t read_pressure_func(struct bt_conn* conn, uint8_t err, struct bt_gatt_read_params* params,
                                  const void* data, uint16_t length)
{
    if (err || !data || length != 2) {
        LOG_ERR("Read Pressure failed (err %d)", err);
        params->single.handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t pressure = ((int16_t*)data)[0];
    if (pressure_callback != NULL) {
        pressure_callback(pressure);
    }

    LOG_INF("[READ] Pressure: %ihPa", pressure);

    return BT_GATT_ITER_STOP;
}

static uint8_t notify_pressure_func(struct bt_conn* conn, struct bt_gatt_subscribe_params* params, const void* data,
                                    uint16_t length)
{
    if (!data || length != 2) {
        LOG_INF("[UNSUBSCRIBED] Pressure");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    int16_t pressure = ((int16_t*)data)[0];
    if (pressure_callback != NULL) {
        pressure_callback(pressure);
    }

    LOG_INF("[NOTIFICATION] Pressure: %ihPa", pressure);

    return BT_GATT_ITER_CONTINUE;
}

/* Discover BAS */

static void discover_bas(void)
{
    discover_params.uuid = UUID_BAS;
    discover_params.func = discover_bas_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    discover_step = 0;
    int err = bt_gatt_discover(default_conn, &discover_params);
    if (err) {
        LOG_ERR("Discover BAS failed (err %d)", err);
    }
}

static uint8_t discover_bas_func(struct bt_conn* conn, const struct bt_gatt_attr* attr,
                                 struct bt_gatt_discover_params* params)
{
    int err;

    if (conn != default_conn || !attr) {
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    if (discover_step == 0) {
        params->uuid = UUID_BAS_BATTERY_LEVEL;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Battery Level failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 1) {
        read_battery_level_params.handle_count = 1;
        read_battery_level_params.single.handle = bt_gatt_attr_value_handle(attr);
        read_battery_level_params.single.offset = 0;
        read_battery_level_params.func = read_battery_level_func;

        err = bt_gatt_read(conn, &read_battery_level_params);
        if (err) {
            LOG_ERR("Read Battery Level failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }

        params->uuid = UUID_GATT_CCC;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_DESCRIPTOR;
        subscribe_battery_level_params.value_handle = bt_gatt_attr_value_handle(attr);

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Battery Level CCC failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 2) {
        subscribe_battery_level_params.notify = notify_battery_level_func;
        subscribe_battery_level_params.value = BT_GATT_CCC_NOTIFY;
        subscribe_battery_level_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &subscribe_battery_level_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe Battery Level failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
        LOG_INF("[SUBSCRIBED] Battery Level");

        discover_ess();
    }

    return BT_GATT_ITER_STOP;
}

/* Discover ESS */

static void discover_ess(void)
{
    discover_params.uuid = UUID_ESS;
    discover_params.func = discover_ess_func;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    discover_step = 0;
    int err = bt_gatt_discover(default_conn, &discover_params);
    if (err) {
        LOG_ERR("Discover ESS failed (err %d)", err);
    }
}

static uint8_t discover_ess_func(struct bt_conn* conn, const struct bt_gatt_attr* attr,
                                 struct bt_gatt_discover_params* params)
{
    int err;

    if (conn != default_conn || !attr) {
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    if (discover_step == 0) {
        params->uuid = UUID_TEMPERATURE;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Temperature failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 1) {
        read_temperature_params.handle_count = 1;
        read_temperature_params.single.handle = bt_gatt_attr_value_handle(attr);
        read_temperature_params.single.offset = 0;
        read_temperature_params.func = read_temperature_func;

        err = bt_gatt_read(conn, &read_temperature_params);
        if (err) {
            LOG_ERR("Read Temperature failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }

        params->uuid = UUID_GATT_CCC;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_DESCRIPTOR;
        subscribe_temperature_params.value_handle = bt_gatt_attr_value_handle(attr);

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Temperature CCC failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 2) {
        subscribe_temperature_params.notify = notify_temperature_func;
        subscribe_temperature_params.value = BT_GATT_CCC_NOTIFY;
        subscribe_temperature_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &subscribe_temperature_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe Temperature failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
        LOG_INF("[SUBSCRIBED] Temperature");

        params->uuid = UUID_HUMIDITY;
        params->start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Humidity failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 3) {
        read_humidity_params.handle_count = 1;
        read_humidity_params.single.handle = bt_gatt_attr_value_handle(attr);
        read_humidity_params.single.offset = 0;
        read_humidity_params.func = read_humidity_func;

        err = bt_gatt_read(conn, &read_humidity_params);
        if (err) {
            LOG_ERR("Read Humidity failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }

        params->uuid = UUID_GATT_CCC;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_DESCRIPTOR;
        subscribe_humidity_params.value_handle = bt_gatt_attr_value_handle(attr);

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Humidity CCC failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 4) {
        subscribe_humidity_params.notify = notify_humidity_func;
        subscribe_humidity_params.value = BT_GATT_CCC_NOTIFY;
        subscribe_humidity_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &subscribe_humidity_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe Humidity failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
        LOG_INF("[SUBSCRIBED] Humidity");

        params->uuid = UUID_PRESSURE;
        params->start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
        params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Pressure failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 5) {
        read_pressure_params.handle_count = 1;
        read_pressure_params.single.handle = bt_gatt_attr_value_handle(attr);
        read_pressure_params.single.offset = 0;
        read_pressure_params.func = read_pressure_func;

        err = bt_gatt_read(conn, &read_pressure_params);
        if (err) {
            LOG_ERR("Read Pressure failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }

        params->uuid = UUID_GATT_CCC;
        params->start_handle = attr->handle + 1;
        params->type = BT_GATT_DISCOVER_DESCRIPTOR;
        subscribe_pressure_params.value_handle = bt_gatt_attr_value_handle(attr);

        discover_step++;
        err = bt_gatt_discover(conn, params);
        if (err) {
            LOG_ERR("Discover Pressure CCC failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
    } else if (discover_step == 6) {
        subscribe_pressure_params.notify = notify_pressure_func;
        subscribe_pressure_params.value = BT_GATT_CCC_NOTIFY;
        subscribe_pressure_params.ccc_handle = attr->handle;

        err = bt_gatt_subscribe(conn, &subscribe_pressure_params);
        if (err && err != -EALREADY) {
            LOG_ERR("Subscribe Pressure failed (err %d)", err);
            return BT_GATT_ITER_STOP;
        }
        LOG_INF("[SUBSCRIBED] Pressure");
    }

    return BT_GATT_ITER_STOP;
}

static bool eir_found(struct bt_data* data, void* user_data)
{
    bt_addr_le_t* addr = user_data;
    struct bt_conn_le_create_param* create_param;
    struct bt_le_conn_param* param;
    struct bt_uuid_16 uuid16;
    bool bas_found = false;
    bool ess_found = false;
    int err;
    int i;

    switch (data->type) {
        case BT_DATA_UUID16_SOME:
        case BT_DATA_UUID16_ALL:
            if (data->data_len % BT_UUID_SIZE_16 != 0U) {
                LOG_INF("AD malformed");
                return true;
            }

            for (i = 0; i < data->data_len; i += BT_UUID_SIZE_16) {
                if (!bt_uuid_create(&uuid16.uuid, &data->data[i], BT_UUID_SIZE_16)) {
                    return true;
                }
                if (!bt_uuid_cmp(&uuid16.uuid, UUID_BAS)) {
                    bas_found = true;
                } else if (!bt_uuid_cmp(&uuid16.uuid, UUID_ESS)) {
                    ess_found = true;
                }
            }

            if (!bas_found || !ess_found) {
                return true;
            }

            LOG_INF("Stop scanning");
            err = bt_le_scan_stop();
            if (err) {
                LOG_ERR("Stop LE scan failed (err %d)", err);
                return true;
            }

            LOG_INF("Creating connection");
            param = BT_LE_CONN_PARAM_DEFAULT;
            create_param = BT_CONN_LE_CREATE_CONN;
            err = bt_conn_le_create(addr, create_param, param, &default_conn);
            if (err) {
                LOG_ERR("Create connection failed (err %d)", err);
                bt_central_start_scan();
            }

            return false;
    }

    return true;
}

static void device_found(const bt_addr_le_t* addr, int8_t rssi, uint8_t type, struct net_buf_simple* ad)
{
    char dev[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(addr, dev, sizeof(dev));

    if (type == BT_GAP_ADV_TYPE_ADV_IND || type == BT_GAP_ADV_TYPE_ADV_DIRECT_IND || type == BT_GAP_ADV_TYPE_EXT_ADV) {
        bt_data_parse(ad, eir_found, (void*)addr);
    }
}

static void connected(struct bt_conn* conn, uint8_t conn_err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (%u)", addr, conn_err);

        bt_conn_unref(default_conn);
        default_conn = NULL;

        bt_central_start_scan();
        return;
    }

    LOG_INF("Connected: %s", addr);
    if (connection_callback != NULL) {
        connection_callback(true);
    }

    if (conn == default_conn) {
        discover_bas();
    }
}

static void disconnected(struct bt_conn* conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_INF("Disconnected: %s, reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));
    if (connection_callback != NULL) {
        connection_callback(false);
    }

    if (default_conn != conn) {
        return;
    }

    bt_conn_unref(default_conn);
    default_conn = NULL;
}

static void recycled()
{
    bt_central_start_scan();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

int bt_central_start_scan(void)
{
    int err;

    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_param, device_found);
    if (err) {
        LOG_INF("Scanning failed to start (err %d)", err);
        return err;
    }

    LOG_INF("Scanning started");

    return 0;
}

void bt_central_register_battery_level_callback(device_battery_level_callback_t callback)
{
    battery_level_callback = callback;
}

void bt_central_register_temperature_callback(device_temperature_callback_t callback)
{
    temperature_callback = callback;
}

void bt_central_register_humidity_callback(device_humidity_callback_t callback)
{
    humidity_callback = callback;
}

void bt_central_register_pressure_callback(device_pressure_callback_t callback)
{
    pressure_callback = callback;
}

void bt_central_register_connection_callback(device_connection_callback_t callback)
{
    connection_callback = callback;
}
