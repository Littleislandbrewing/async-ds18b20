import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_PIN
from esphome import pins

CODEOWNERS = ["@Littleislandbrewing"]
DEPENDENCIES = []
AUTO_LOAD = []
MULTI_CONF = True

async_dallas_ns = cg.esphome_ns.namespace('async_dallas')
AsyncDallasComponent = async_dallas_ns.class_('AsyncDallasComponent', cg.Component)

CONF_ASYNC_DALLAS_ID = "async_dallas_id"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AsyncDallasComponent),
    cv.Required(CONF_PIN): pins.internal_gpio_input_pin_schema,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    # No external libraries — pure ESP-IDF GPIO implementation
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
