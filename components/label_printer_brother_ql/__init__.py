"""Brother QL raster protocol (a label_printer driver).

The 720-dot, 300 dpi QL models (QL-500 to QL-580N, QL-700, QL-710W,
QL-720NW, QL-800 to QL-820NWB) with a continuous DK roll. Pair it with a
transport (label_printer_tcp for a printer on the network) in a
label_printer. See espoolbuddy/label_printer_brother_ql.yaml.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL

from esphome.components.label_printer import LabelDriver

CODEOWNERS = ["@CSchlipp"]
DEPENDENCIES = ["label_printer"]

CONF_MEDIA = "media"
CONF_CUT = "cut"

label_printer_brother_ql_ns = cg.esphome_ns.namespace("label_printer_brother_ql")
BrotherQLDriver = label_printer_brother_ql_ns.class_("BrotherQLDriver", LabelDriver)
Media = BrotherQLDriver.enum("Media")

# Continuous (DK endless) roll widths in mm.
MEDIA = {
    "12": Media.MEDIA_12,
    "29": Media.MEDIA_29,
    "38": Media.MEDIA_38,
    "50": Media.MEDIA_50,
    "54": Media.MEDIA_54,
    "62": Media.MEDIA_62,
}


def media(value):
    value = cv.string(value).strip().lower()
    if value.endswith("mm"):
        value = value[:-2].strip()
    return cv.enum(MEDIA)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BrotherQLDriver),
        # For the log only: every 720-dot QL speaks the same protocol.
        cv.Optional(CONF_MODEL, default="Brother QL"): cv.string_strict,
        cv.Optional(CONF_MEDIA, default="62"): media,
        cv.Optional(CONF_CUT, default=True): cv.boolean,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_media(config[CONF_MEDIA]))
    cg.add(var.set_cut(config[CONF_CUT]))
