"""Desktop simulator label printer: writes each label to a PBM image file.

The driver and the transport for label_printer in one, for the host
platform only. See host/espoolbuddy_console_host_labels.yaml.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.label_printer import LabelDriver, LabelTransport
from esphome.const import CONF_ID, CONF_PATH

CODEOWNERS = ["@CSchlipp"]
DEPENDENCIES = ["label_printer"]

CONF_HEAD_DOTS = "head_dots"


def _whole_bytes(value):
    if value % 8:
        raise cv.Invalid("head_dots must be a multiple of 8")
    return value

label_printer_file_ns = cg.esphome_ns.namespace("label_printer_file")
LabelPrinterFile = label_printer_file_ns.class_(
    "LabelPrinterFile", cg.Component, LabelDriver, LabelTransport
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LabelPrinterFile),
            cv.Optional(CONF_PATH, default="label.pbm"): cv.string,
            # The print row to place the label in; 576 is the M220's head.
            cv.Optional(CONF_HEAD_DOTS, default=576): cv.All(
                cv.int_range(min=8, max=2048), _whole_bytes
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(["host"]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_path(config[CONF_PATH]))
    cg.add(var.set_head_dots(config[CONF_HEAD_DOTS]))
