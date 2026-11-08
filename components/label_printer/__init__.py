"""Label printer core: prints a spool's label on a nearby label printer.

Bambuddy draws the label (POST /inventory/labels as a PNG at the printer's
resolution); this component turns the PNG into a 1-bit raster and hands it to
a driver (the printer's protocol: label_printer_phomemo) over a transport
(how the bytes travel: label_printer_ble). The UI talks only to this component. See
espoolbuddy/label_printer_m220.yaml for a package that wires the three together.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@CSchlipp"]

label_printer_ns = cg.esphome_ns.namespace("label_printer")
LabelPrinter = label_printer_ns.class_("LabelPrinter", cg.Component)
LabelDriver = label_printer_ns.class_("LabelDriver")
LabelTransport = label_printer_ns.class_("LabelTransport")

bambuddy_api_ns = cg.esphome_ns.namespace("bambuddy_api")
BambuddyAPIComponent = bambuddy_api_ns.class_("BambuddyAPIComponent", cg.Component)

CONF_API_ID = "api_id"
CONF_DRIVER_ID = "driver_id"
CONF_TRANSPORT_ID = "transport_id"
CONF_LABEL_TEMPLATE = "label_template"
CONF_X_OFFSET = "x_offset"
CONF_LABEL_FIELDS = "label_fields"

# Bambuddy's single-label templates, width across the head by length along
# the feed. Both fit a 72 mm head. A label a little wider than the printer
# prints (62 mm on a 62 mm roll whose printable area is narrower) is shrunk
# to fit when it arrives; one far too wide is refused.
LABEL_TEMPLATES = ["box_40x30", "box_62x29"]

# Bambuddy's label lines (LabelField in its label_renderer.py), top to bottom.
LABEL_FIELDS = [
    "brand",
    "material",
    "hex",
    "name",
    "location",
    "material_number",
    "temps",
    "weight",
    "note",
    "added",
    "qr",
    "spool_id",
]


def label_fields(value):
    """A list, or a comma-separated string (so it fits in a substitution)."""
    if isinstance(value, str):
        value = [v for v in (p.strip() for p in value.split(",")) if v]
    value = cv.ensure_list(cv.one_of(*LABEL_FIELDS, lower=True))(value)
    if len(set(value)) != len(value):
        raise cv.Invalid("label_fields lists a field twice")
    return value


# Against a wall: beyond any head, clamped to the row's edge when printed, so
# it works for every head and label width.
X_OFFSET_MAX = 2048
X_OFFSET_WALLS = {"center": 0, "right": X_OFFSET_MAX, "left": -X_OFFSET_MAX}


def x_offset(value):
    if isinstance(value, str) and value.strip().lower() in X_OFFSET_WALLS:
        return X_OFFSET_WALLS[value.strip().lower()]
    return cv.int_range(min=-X_OFFSET_MAX, max=X_OFFSET_MAX)(cv.int_(value))


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LabelPrinter),
        cv.Required(CONF_API_ID): cv.use_id(BambuddyAPIComponent),
        cv.Required(CONF_DRIVER_ID): cv.use_id(LabelDriver),
        cv.Required(CONF_TRANSPORT_ID): cv.use_id(LabelTransport),
        cv.Optional(CONF_LABEL_TEMPLATE, default="box_40x30"): cv.one_of(
            *LABEL_TEMPLATES, lower=True
        ),
        # Where the roll runs under the head: "right", "left" or "center", or
        # dots right of centre, negative for left. The M220's holder keeps a
        # roll narrower than the head against its right wall, so that's the
        # default.
        cv.Optional(CONF_X_OFFSET, default="right"): x_offset,
        # The lines to print; empty (the default) is whatever Bambuddy
        # prints by default.
        cv.Optional(CONF_LABEL_FIELDS, default=[]): label_fields,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_api(await cg.get_variable(config[CONF_API_ID])))
    cg.add(var.set_driver(await cg.get_variable(config[CONF_DRIVER_ID])))
    cg.add(var.set_transport(await cg.get_variable(config[CONF_TRANSPORT_ID])))
    cg.add(var.set_label_template(config[CONF_LABEL_TEMPLATE]))
    cg.add(var.set_x_offset(config[CONF_X_OFFSET]))
    cg.add(var.set_label_fields(",".join(config[CONF_LABEL_FIELDS])))
    cg.add_define("USE_LABEL_PRINTER")
    cg.add_library("pngle", "1.1.0")
