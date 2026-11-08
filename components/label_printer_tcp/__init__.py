"""Network transport for label_printer: a raw TCP stream (port 9100).

Connects to the printer for a print and closes after it. The host is an IPv4
address or a hostname. Pair it with a driver (label_printer_brother_ql) in a
label_printer.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import socket
from esphome.components.label_printer import LabelTransport
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@CSchlipp"]
DEPENDENCIES = ["label_printer"]
AUTO_LOAD = ["socket"]

CONF_HOST = "host"


def printer_host(value):
    """The printer's address; empty is the package's way of saying it's unset."""
    if value is None or not str(value).strip():
        raise cv.Invalid(
            "No printer address: set the label_printer_host substitution to the "
            "printer's IPv4 address or hostname, e.g. label_printer_host: \"192.168.1.50\""
        )
    return socket.ipv4_host(str(value).strip())

label_printer_tcp_ns = cg.esphome_ns.namespace("label_printer_tcp")
LabelPrinterTCP = label_printer_tcp_ns.class_(
    "LabelPrinterTCP", cg.Component, LabelTransport
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LabelPrinterTCP),
            cv.Required(CONF_HOST): printer_host,
            cv.Optional(CONF_PORT, default=9100): cv.port,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    socket.consume_sockets(1, "label_printer_tcp"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    socket.require_tcp_client_link()
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
