"""ESPHome component schema for BambuddyNFC (PN532 NFC reader with Bambu MIFARE support)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import spi
from esphome import pins
from esphome.const import CONF_ID

CODEOWNERS = ["@CSchlipp"]
MULTI_CONF = False
DEPENDENCIES = ["spi"]
AUTO_LOAD = []

bambuddy_nfc_ns = cg.esphome_ns.namespace("bambuddy_nfc")
BambuddyNFCComponent = bambuddy_nfc_ns.class_(
    "BambuddyNFCComponent",
    cg.Component,
    spi.SPIDevice,
)

# Import BambuddyAPIComponent type for the api_id reference
bambuddy_api_ns = cg.esphome_ns.namespace("bambuddy_api")
BambuddyAPIComponent = bambuddy_api_ns.class_("BambuddyAPIComponent")

CONF_API_ID = "api_id"
CONF_POLL_INTERVAL = "poll_interval"
CONF_MISS_THRESHOLD = "miss_threshold"
CONF_IRQ_PIN = "irq_pin"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BambuddyNFCComponent),
            cv.Required(CONF_API_ID): cv.use_id(BambuddyAPIComponent),
            cv.Optional(CONF_POLL_INTERVAL, default=300): cv.positive_int,
            cv.Optional(CONF_MISS_THRESHOLD, default=3): cv.positive_int,
            cv.Optional(CONF_IRQ_PIN): pins.gpio_input_pin_schema,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True, default_data_rate="1MHz"))
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "bambuddy_nfc",
    require_mosi=True,
    require_miso=True,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)

    api = await cg.get_variable(config[CONF_API_ID])
    cg.add(var.set_api_component(api))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_miss_threshold(config[CONF_MISS_THRESHOLD]))

    if CONF_IRQ_PIN in config:
        irq = await cg.gpio_pin_expression(config[CONF_IRQ_PIN])
        cg.add(var.set_irq_pin(irq))
