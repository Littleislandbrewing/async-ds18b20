import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_PIN, CONF_UPDATE_INTERVAL, CONF_ID

# Namespace
async_dallas_ns = cg.esphome_ns.namespace('async_dallas')
AsyncDallasSensor = async_dallas_ns.class_('AsyncDallasSensor', sensor.Sensor, cg.PollingComponent)

CONFIG_SCHEMA = sensor.sensor_schema(
    AsyncDallasSensor,
    unit_of_measurement="°C",
    accuracy_decimals=2,
).extend({
    cv.Required(CONF_PIN): cv.uint8_t,
    cv.Optional(CONF_UPDATE_INTERVAL, default="1s"): cv.update_interval,
})

async def to_code(config):
    # Libraries - use newer versions compatible with ESP-IDF
    cg.add_library("paulstoffregen/OneWire", "2.3.8")
    cg.add_library("milesburton/DallasTemperature", "3.11.0")

    var = cg.new_Pvariable(config[CONF_ID], config[CONF_PIN], config[CONF_UPDATE_INTERVAL])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)
