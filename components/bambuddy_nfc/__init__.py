"""ESPHome component schema for BambuddyNFC (PN532 NFC reader with Bambu MIFARE support).

One component, two host interfaces: `interface: spi` (default, so configs
written before I2C support keep working unchanged) or `interface: i2c`. Both
generate a subclass of BambuddyNFCComponent, which registers itself with the
bambuddy_api component named by api_id — the API side never needs to know
which bus is in use.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, spi
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@CSchlipp"]
MULTI_CONF = False
# No DEPENDENCIES: which bus is needed depends on `interface:`. A missing
# spi:/i2c: bus is still reported at validation time, by the bus schema's
# spi_id/i2c_id reference.
AUTO_LOAD = []

bambuddy_nfc_ns = cg.esphome_ns.namespace("bambuddy_nfc")
BambuddyNFCComponent = bambuddy_nfc_ns.class_("BambuddyNFCComponent", cg.Component)
BambuddyNFCSPIComponent = bambuddy_nfc_ns.class_(
    "BambuddyNFCSPIComponent",
    BambuddyNFCComponent,
    spi.SPIDevice,
)
BambuddyNFCI2CComponent = bambuddy_nfc_ns.class_(
    "BambuddyNFCI2CComponent",
    BambuddyNFCComponent,
    i2c.I2CDevice,
)

# Import BambuddyAPIComponent type for the api_id reference
bambuddy_api_ns = cg.esphome_ns.namespace("bambuddy_api")
BambuddyAPIComponent = bambuddy_api_ns.class_("BambuddyAPIComponent")

CONF_API_ID = "api_id"
CONF_INTERFACE = "interface"
CONF_POLL_INTERVAL = "poll_interval"
CONF_MISS_THRESHOLD = "miss_threshold"
CONF_IRQ_PIN = "irq_pin"

INTERFACE_SPI = "spi"
INTERFACE_I2C = "i2c"

# PN532 default 7-bit I2C address
PN532_I2C_ADDRESS = 0x24

BASE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_API_ID): cv.use_id(BambuddyAPIComponent),
        cv.Optional(CONF_POLL_INTERVAL, default=300): cv.positive_int,
        cv.Optional(CONF_MISS_THRESHOLD, default=3): cv.positive_int,
        cv.Optional(CONF_IRQ_PIN): pins.gpio_input_pin_schema,
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.typed_schema(
    {
        INTERFACE_SPI: BASE_SCHEMA.extend(
            {cv.GenerateID(): cv.declare_id(BambuddyNFCSPIComponent)}
        ).extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate="1MHz")),
        INTERFACE_I2C: BASE_SCHEMA.extend(
            {cv.GenerateID(): cv.declare_id(BambuddyNFCI2CComponent)}
        ).extend(i2c.i2c_device_schema(PN532_I2C_ADDRESS)),
    },
    key=CONF_INTERFACE,
    default_type=INTERFACE_SPI,
    lower=True,
)

_SPI_FINAL_VALIDATE = spi.final_validate_device_schema(
    "bambuddy_nfc",
    require_mosi=True,
    require_miso=True,
)
# The PN532 supports I2C fast mode at most.
_I2C_FINAL_VALIDATE = i2c.final_validate_device_schema(
    "bambuddy_nfc",
    max_frequency="400kHz",
)


def FINAL_VALIDATE_SCHEMA(config):
    if config[CONF_INTERFACE] == INTERFACE_SPI:
        return _SPI_FINAL_VALIDATE(config)
    return _I2C_FINAL_VALIDATE(config)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Only the selected transport's sources compile (see the #ifdef in
    # bambuddy_nfc_spi.h / bambuddy_nfc_i2c.h) — the other bus component may
    # not even be part of this build.
    if config[CONF_INTERFACE] == INTERFACE_SPI:
        cg.add_define("USE_BAMBUDDY_NFC_SPI")
        await spi.register_spi_device(var, config)
    else:
        cg.add_define("USE_BAMBUDDY_NFC_I2C")
        await i2c.register_i2c_device(var, config)

    # Link both ways from this one api_id: the reader reports scans to the API
    # component, and registers itself there so the API side can drive NFC
    # sleep behaviour and report the reader (has_nfc, nfc_connection).
    api = await cg.get_variable(config[CONF_API_ID])
    cg.add(var.set_api_component(api))
    cg.add(api.set_nfc_component(var))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_miss_threshold(config[CONF_MISS_THRESHOLD]))

    if CONF_IRQ_PIN in config:
        irq = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
        cg.add(var.set_irq_pin(irq))
