"""ESPHome component schema for BambuddyAPI (SpoolBuddy backend client)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.light import LightState
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@CSchlipp"]
MULTI_CONF = False
DEPENDENCIES = ["network", "wifi"]
AUTO_LOAD = []

bambuddy_api_ns = cg.esphome_ns.namespace("bambuddy_api")
BambuddyAPIComponent = bambuddy_api_ns.class_("BambuddyAPIComponent", cg.Component)

# Forward-declared stubs for optional cross-component ids — avoids importing
# bambuddy_nfc's/rtttl's __init__.py (which would create a circular import,
# since bambuddy_nfc's own schema already depends on BambuddyAPIComponent).
bambuddy_nfc_ns = cg.esphome_ns.namespace("bambuddy_nfc")
BambuddyNFCComponent = bambuddy_nfc_ns.class_("BambuddyNFCComponent")

rtttl_ns = cg.esphome_ns.namespace("rtttl")
RtttlComponent = rtttl_ns.class_("Rtttl")

CONF_BACKEND_URL = "backend_url"
CONF_API_KEY = "api_key"
CONF_DEVICE_ID = "device_id"
CONF_HOSTNAME = "hostname"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_SCALE_REPORT_INTERVAL = "scale_report_interval"
CONF_PRINTER_POLL_INTERVAL = "printer_poll_interval"
CONF_SCALE_MODE = "scale_mode"
CONF_CONSOLE_URL = "console_url"
CONF_SLEEP_TIMEOUT = "sleep_timeout"
CONF_SLEEP_FACTOR = "sleep_factor"
CONF_INVENTORY_BACKEND = "inventory_backend"
CONF_CLOCK_24H = "clock_24h"
CONF_NFC_ID = "nfc_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_BACKLIGHT_ID = "backlight_id"

INVENTORY_BACKEND_INTERNAL = "internal"
INVENTORY_BACKEND_SPOOLMAN = "spoolman"


def _validate_backlight_required(config):
    # Every non-scale device has a physical display + backlight; scale_mode
    # devices are headless and have neither, so only require it there.
    if not config[CONF_SCALE_MODE] and CONF_BACKLIGHT_ID not in config:
        raise cv.Invalid("backlight_id is required unless scale_mode: true")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BambuddyAPIComponent),
            # backend_url / api_key are not needed on scale devices (scale_mode: true)
            cv.Optional(CONF_BACKEND_URL, default=""): cv.string,
            cv.Optional(CONF_API_KEY, default=""): cv.string,
            # Which Bambuddy inventory backend to talk to. Bambuddy can be configured
            # to track spools in its own local DB ("internal") or delegate to a
            # Spoolman instance ("spoolman") — the two expose different endpoint
            # shapes (e.g. /inventory/... vs /spoolman/inventory/...), so this is a
            # compile-time choice matching whatever Bambuddy itself is set to use
            # (Settings → Spoolman in the Bambuddy UI).
            cv.Optional(CONF_INVENTORY_BACKEND, default=INVENTORY_BACKEND_INTERNAL): cv.one_of(
                INVENTORY_BACKEND_INTERNAL, INVENTORY_BACKEND_SPOOLMAN, lower=True
            ),
            cv.Optional(CONF_DEVICE_ID, default=""): cv.string,
            cv.Optional(CONF_HOSTNAME, default="SpoolBuddy-ESP"): cv.string,
            cv.Optional(CONF_HEARTBEAT_INTERVAL, default=10): cv.positive_int,
            cv.Optional(CONF_SCALE_REPORT_INTERVAL, default=1000): cv.positive_int,
            cv.Optional(CONF_PRINTER_POLL_INTERVAL, default=30): cv.positive_int,
            # Scale mode: this device IS the scale — starts a local HTTP server for
            # tare/calibrate and pushes weight/NFC events to the console.
            # Skips all BamBuddy communication entirely.
            # backend_url and api_key are ignored when this is true.
            cv.Optional(CONF_SCALE_MODE, default=False): cv.boolean,
            # Scale device only: base URL(s) of the console(s) that will receive push
            # data — no port suffix (e.g. "http://spoolbuddy-console.local"). Accepts
            # either a single string or a list; when multiple are given, the first is
            # authoritative (its heartbeat response is the only one honored for
            # tare/calibrate/write_tag commands, and it alone drives the connectivity
            # LED) while the rest are push-only observers. The port (CONSOLE_PUSH_PORT)
            # is applied automatically in the component. Requires scale_mode: true.
            cv.Optional(CONF_CONSOLE_URL, default=[]): cv.ensure_list(cv.string),
            # Sleep / deep-idle (console only): seconds of UI inactivity before the
            # backlight is switched off and the backend cadence is reduced. 0 = off.
            cv.Optional(CONF_SLEEP_TIMEOUT, default=600): cv.positive_int,
            # Multiplier applied to the heartbeat and printer-poll intervals while
            # asleep (e.g. 6 turns a 10 s heartbeat into 60 s).
            cv.Optional(CONF_SLEEP_FACTOR, default=6): cv.int_range(min=1),
            # Console only: header clock format. true = 24-hour (14:05), false = 12-hour (2:05 PM).
            cv.Optional(CONF_CLOCK_24H, default=True): cv.boolean,
            # Optional hardware this device may not have — when omitted, the
            # component gracefully no-ops calls that would otherwise target it
            # and reports its absence honestly to the backend.
            cv.Optional(CONF_NFC_ID): cv.use_id(BambuddyNFCComponent),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(RtttlComponent),
            # Mandatory for any console device (see _validate_backlight_required
            # below) — every display-having device has a real backlight; only
            # headless scale_mode devices are exempt.
            cv.Optional(CONF_BACKLIGHT_ID): cv.use_id(LightState),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_backlight_required,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_backend_url(config[CONF_BACKEND_URL]))
    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_hostname(config[CONF_HOSTNAME]))
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_scale_report_interval(config[CONF_SCALE_REPORT_INTERVAL]))
    cg.add(var.set_printer_poll_interval(config[CONF_PRINTER_POLL_INTERVAL]))
    cg.add(var.set_scale_mode(config[CONF_SCALE_MODE]))
    for url in config[CONF_CONSOLE_URL]:
        cg.add(var.add_console_url(url))
    cg.add(var.set_sleep_timeout(config[CONF_SLEEP_TIMEOUT]))
    cg.add(var.set_sleep_factor(config[CONF_SLEEP_FACTOR]))
    cg.add(var.set_spoolman_inventory(config[CONF_INVENTORY_BACKEND] == INVENTORY_BACKEND_SPOOLMAN))
    cg.add(var.set_clock_24h(config[CONF_CLOCK_24H]))
    if CONF_NFC_ID in config:
        nfc = await cg.get_variable(config[CONF_NFC_ID])
        cg.add(var.set_nfc_component(nfc))
    if CONF_SPEAKER_ID in config:
        speaker = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker_component(speaker))
    if CONF_BACKLIGHT_ID in config:
        backlight = await cg.get_variable(config[CONF_BACKLIGHT_ID])
        cg.add(var.set_backlight_component(backlight))
    if CORE.is_esp32:
        from esphome.components.esp32 import include_builtin_idf_component
        include_builtin_idf_component("esp_http_client")
        # Both scale (receive tare/cal) and console (receive push data) use httpd.
        include_builtin_idf_component("esp_http_server")
        if config[CONF_SCALE_MODE]:
            include_builtin_idf_component("nvs_flash")
