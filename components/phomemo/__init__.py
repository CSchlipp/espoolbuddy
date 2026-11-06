"""Phomemo M series label printer protocol (a label_printer driver).

Only the M220 is supported: 72 mm head at 203 dpi, Bluetooth service ff00.
Pair it with a transport (label_printer_ble) in a label_printer.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL

from esphome.components.label_printer import LabelDriver

CODEOWNERS = ["@CSchlipp"]
DEPENDENCIES = ["label_printer"]

phomemo_ns = cg.esphome_ns.namespace("phomemo")
PhomemoDriver = phomemo_ns.class_("PhomemoDriver", LabelDriver)
Model = PhomemoDriver.enum("Model")

MODELS = {"m220": Model.M220}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PhomemoDriver),
        cv.Optional(CONF_MODEL, default="m220"): cv.enum(MODELS, lower=True),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_model(config[CONF_MODEL]))
