import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_PIN, CONF_UPDATE_INTERVAL, CONF_ID

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
    # Correct Git URL format for PlatformIO
    cg.add_library(None, None, "https://github.com/mathieucarbou/OneWire.git")
    cg.add_library(None, None, "https://github.com/mathieucarbou/Arduino-Temperature-Control-Library.git")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await sensor.register_sensor(var, config)
    
    cg.add(var.set_pin(config[CONF_PIN]))
