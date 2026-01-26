import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_PIN
from esphome import pins

CODEOWNERS = ["@yourusername"]
DEPENDENCIES = []
AUTO_LOAD = []

async_dallas_ns = cg.esphome_ns.namespace('async_dallas')
AsyncDallasComponent = async_dallas_ns.class_('AsyncDallasComponent', cg.PollingComponent)

CONF_ASYNC_DALLAS_ID = "async_dallas_id"

# Single hub config schema
SINGLE_HUB_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AsyncDallasComponent),
    cv.Required(CONF_PIN): pins.gpio_input_pin_schema,
}).extend(cv.polling_component_schema('1s'))

# Support multiple hubs via list
CONFIG_SCHEMA = cv.All(cv.ensure_list(SINGLE_HUB_SCHEMA))

async def to_code(config):
    # Add libraries with the slash format that worked before
    cg.add_library("paulstoffregen/OneWire", "2.3.8")
    cg.add_library("milesburton/DallasTemperature", "3.11.0")
    
    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)
        
        pin = await cg.gpio_pin_expression(conf[CONF_PIN])
        cg.add(var.set_pin(pin))
