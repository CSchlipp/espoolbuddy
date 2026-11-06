"""Bluetooth LE transport for label_printer.

Connects to the printer through a ble_client, using the driver's GATT
characteristics, and switches Bluetooth on only while a label prints. With
the ble_client's mac_address left at 00:00:00:00:00:00 it finds the printer
by the driver's advertisement rule on the first print, and remembers it.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, esp32_ble_tracker
from esphome.components.label_printer import LabelTransport
from esphome.const import CONF_ID

CODEOWNERS = ["@CSchlipp"]
DEPENDENCIES = ["label_printer", "ble_client", "esp32_ble_tracker", "psram"]

label_printer_ble_ns = cg.esphome_ns.namespace("label_printer_ble")
LabelPrinterBLE = label_printer_ble_ns.class_(
    "LabelPrinterBLE",
    cg.Component,
    ble_client.BLEClientNode,
    esp32_ble_tracker.ESPBTDeviceListener,
    LabelTransport,
)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(LabelPrinterBLE)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Sees advertisements, to find the printer when no address is configured.
    await esp32_ble_tracker.register_ble_device(var, config)
