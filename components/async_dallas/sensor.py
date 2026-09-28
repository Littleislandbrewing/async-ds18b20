import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_INDEX,
    CONF_RESOLUTION,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)
from . import AsyncDallasComponent, async_dallas_ns, CONF_ASYNC_DALLAS_ID

AsyncDallasSensor = async_dallas_ns.class_('AsyncDallasSensor', sensor.Sensor)

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(
        AsyncDallasSensor,
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ).extend({
        cv.GenerateID(CONF_ASYNC_DALLAS_ID): cv.use_id(AsyncDallasComponent),
        cv.Optional(CONF_ADDRESS): cv.hex_uint64_t,
        cv.Optional(CONF_INDEX, default=0): cv.positive_int,
        cv.Optional(CONF_RESOLUTION, default=9): cv.int_range(min=9, max=12),
        cv.Optional(CONF_UPDATE_INTERVAL, default="10s"): cv.update_interval,
    }),
    cv.has_at_most_one_key(CONF_ADDRESS, CONF_INDEX),
)

async def to_code(config):
    hub = await cg.get_variable(config[CONF_ASYNC_DALLAS_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    await sensor.register_sensor(var, config)

    # Pass update interval to hub (in milliseconds)
    update_interval_ms = int(config[CONF_UPDATE_INTERVAL].total_milliseconds)
    cg.add(hub.set_update_interval(update_interval_ms))

    if CONF_ADDRESS in config:
        cg.add(var.set_address(config[CONF_ADDRESS]))
    else:
        cg.add(var.set_index(config[CONF_INDEX]))

    cg.add(var.set_resolution(config[CONF_RESOLUTION]))
    cg.add(var.set_parent(hub))
    cg.add(hub.register_sensor(var))
