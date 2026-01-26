import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

async_dallas_ns = cg.esphome_ns.namespace('async_dallas')

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(async_dallas_ns.class_('AsyncDallas', cg.Component))
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
```

5. Commit the file

**OR** - if you already have an `__init__.py` file somewhere in your project but it's not showing in the screenshots, make sure it's inside the `async_dallas` folder.

After adding `__init__.py`, your structure will be:
```
components/
└── async_dallas/
    ├── __init__.py          ← REQUIRED
    ├── async_dallas.cpp
    ├── async_dallas.h
    └── sensor.py
