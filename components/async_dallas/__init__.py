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
CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(AsyncDallasComponent),
        cv.Required(CONF_PIN): pins.gpio_input_pin_schema,
    }).extend(cv.polling_component_schema('1s')),
    cv.only_with_arduino,
)

# Support multiple hubs via list
CONFIG_SCHEMA = cv.All(cv.ensure_list(CONFIG_SCHEMA))

async def to_code(config):
    for conf in config:
        var = cg.new_Pvariable(conf[CONF_ID])
        await cg.register_component(var, conf)
        
        pin = await cg.gpio_pin_expression(conf[CONF_PIN])
        cg.add(var.set_pin(pin))
