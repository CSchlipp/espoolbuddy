"""ESPHome component schema for BambuddyAPI (SpoolBuddy backend client)."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor
from esphome.components.light import LightState
from esphome.const import CONF_ID, CONF_SENSOR, CONF_TIMEOUT, CONF_TYPE
from esphome.core import CORE

CODEOWNERS = ["@CSchlipp"]
MULTI_CONF = False
# wifi is only required on ESP32 (see _final_validate below): the host
# platform build (host/espoolbuddy_console_host.yaml) has no wifi component and
# compiles the mock in host/bambuddy_api_mock.cpp instead.
DEPENDENCIES = ["network"]
AUTO_LOAD = []

bambuddy_api_ns = cg.esphome_ns.namespace("bambuddy_api")
BambuddyAPIComponent = bambuddy_api_ns.class_("BambuddyAPIComponent", cg.Component)
ScaleSource = bambuddy_api_ns.enum("ScaleSource", is_class=True)

# Forward-declared stub for the optional speaker id — avoids importing rtttl's
# __init__.py. (The NFC reader needs no id here: bambuddy_nfc registers itself
# with this component through its own api_id.)
rtttl_ns = cg.esphome_ns.namespace("rtttl")
RtttlComponent = rtttl_ns.class_("Rtttl")

CONF_BACKEND_URL = "backend_url"
CONF_API_KEY = "api_key"
CONF_DEVICE_ID = "device_id"
CONF_HOSTNAME = "hostname"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_HEARTBEAT_FAIL_THRESHOLD = "heartbeat_fail_threshold"
CONF_SCALE_REPORT_INTERVAL = "scale_report_interval"
CONF_PRINTER_POLL_INTERVAL = "printer_poll_interval"
CONF_SCALE_MODE = "scale_mode"
CONF_SCALE = "scale"
CONF_STABLE_AFTER = "stable_after"
CONF_STABLE_BAND = "stable_band"
SCALE_TYPE_REMOTE = "remote"
SCALE_TYPE_LOCAL = "local"
SCALE_TYPE_NONE = "none"  # internal only: a satellite without a scale: block
CONF_CONSOLE_URL = "console_url"
CONF_SLEEP_TIMEOUT = "sleep_timeout"
CONF_SLEEP_FACTOR = "sleep_factor"
CONF_SPEAKER_ID = "speaker_id"
CONF_BACKLIGHT_ID = "backlight_id"


def _require_certificate_bundle():
    # ESPHome 2026.9.0 turned the CA bundle into an opt-in that components
    # request, so that builds with no TLS user skip gen_crt_bundle entirely;
    # asking through the helper also pins the smaller common-CA bundle
    # (~51 KB less flash than the full one). Older ESPHome has no such
    # bookkeeping — there the sdkconfig option is the whole mechanism.
    try:
        from esphome.components.esp32 import require_certificate_bundle
    except ImportError:
        from esphome.components.esp32 import add_idf_sdkconfig_option

        add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    else:
        require_certificate_bundle()


def _scale_mode_removed(value):
    raise cv.Invalid(
        "scale_mode was removed in 3.0.0 — the role now follows from the config: "
        "a device with console_url pushes to the console(s), and its load cell is "
        "set with `scale: {type: local, sensor: <sensor id>}`. Also delete the old "
        "HX711 on_value lambda, the stability interval/globals and the "
        "restart_scale_server() call. See docs/scale.md."
    )


# Weight source. One block, one type: Bambuddy keeps a single tare /
# calibration per device and the console reports all weight under its own
# device_id, so two sources on one device would fight over that record.
SCALE_SCHEMA = cv.typed_schema(
    {
        # A separate scale device pushes readings to this console.
        SCALE_TYPE_REMOTE: cv.Schema(
            {
                # Liveness window: no push (heartbeat, weight or NFC event) for
                # this long → "No scale connected".
                cv.Optional(
                    CONF_TIMEOUT, default="10s"
                ): cv.positive_not_null_time_period,
            }
        ),
        # A load-cell sensor wired to this device. The sensor must be linear,
        # not tared, with at least ~1 unit per gram (raw counts or rough grams):
        # the component applies net = (value - tare) * factor itself.
        SCALE_TYPE_LOCAL: cv.Schema(
            {
                cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
                # The reading counts as stable once it hasn't moved by
                # stable_band or more for this long.
                cv.Optional(
                    CONF_STABLE_AFTER, default="750ms"
                ): cv.positive_time_period_milliseconds,
                # In sensor units; matches a `delta: 0.3` filter on the sensor.
                cv.Optional(CONF_STABLE_BAND, default=0.3): cv.positive_float,
                # No raw reading from the sensor for this long → "No scale
                # connected" (unplugged / dead ADC).
                cv.Optional(
                    CONF_TIMEOUT, default="5s"
                ): cv.positive_not_null_time_period,
            }
        ),
    },
    lower=True,
)


def _validate_roles(config):
    satellite = bool(config[CONF_CONSOLE_URL])
    if satellite and config[CONF_BACKEND_URL]:
        raise cv.Invalid(
            "backend_url and console_url are mutually exclusive: a device either "
            "talks to Bambuddy itself (backend_url, a console) or pushes to a "
            "console (console_url, e.g. a scale device)"
        )
    # The host simulator build has neither: it runs a mock backend.
    if CORE.is_esp32 and not satellite and not config[CONF_BACKEND_URL]:
        raise cv.Invalid(
            "set either backend_url (a console talking to Bambuddy) or "
            "console_url (a device pushing to a console)"
        )
    # Consoles have a physical display + backlight; satellites are headless.
    if not satellite and CONF_BACKLIGHT_ID not in config:
        raise cv.Invalid(
            "backlight_id is required on a console (a device without console_url)"
        )
    if CONF_SCALE not in config:
        # Console: a separate scale device may push to it (today's default).
        # Satellite: no load cell — it only forwards NFC events.
        config[CONF_SCALE] = (
            {CONF_TYPE: SCALE_TYPE_NONE}
            if satellite
            else SCALE_SCHEMA({CONF_TYPE: SCALE_TYPE_REMOTE})
        )
    elif satellite and config[CONF_SCALE][CONF_TYPE] == SCALE_TYPE_REMOTE:
        raise cv.Invalid(
            "scale: type: remote is only valid on a console — a device with "
            "console_url can't receive pushes from another scale",
            path=[CONF_SCALE, CONF_TYPE],
        )
    return config


def _final_validate(config):
    if CORE.is_esp32 and "wifi" not in fv.full_config.get():
        raise cv.Invalid("bambuddy_api requires the wifi component on ESP32")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BambuddyAPIComponent),
            # Console only (mutually exclusive with console_url).
            cv.Optional(CONF_BACKEND_URL, default=""): cv.string,
            # cv.sensitive so config dumps redact it deterministically — the
            # legacy "looks like a key" substring heuristic that used to cover
            # this goes away in ESPHome 2026.12.0.
            cv.Optional(CONF_API_KEY, default=""): cv.sensitive(cv.string),
            cv.Optional(CONF_DEVICE_ID, default=""): cv.string,
            cv.Optional(CONF_HOSTNAME, default="SpoolBuddy-ESP"): cv.string,
            cv.Optional(CONF_HEARTBEAT_INTERVAL, default=10): cv.positive_int,
            # Consecutive failed heartbeat POSTs required before the backend is
            # actually marked down (console cloud/WiFi icons turn red). Debounces
            # transient network blips — a single successful heartbeat always
            # clears this immediately, so recovery is never delayed.
            cv.Optional(CONF_HEARTBEAT_FAIL_THRESHOLD, default=3): cv.positive_int,
            cv.Optional(CONF_SCALE_REPORT_INTERVAL, default=1000): cv.positive_int,
            cv.Optional(CONF_PRINTER_POLL_INTERVAL, default=30): cv.positive_int,
            # Removed in 3.0.0 — kept only to point old configs at the migration.
            cv.Optional(CONF_SCALE_MODE): _scale_mode_removed,
            # Weight source — see SCALE_SCHEMA. Default: remote on a console,
            # none on a satellite.
            cv.Optional(CONF_SCALE): SCALE_SCHEMA,
            # Satellite (e.g. scale device): base URL(s) of the console(s) that will receive push
            # data — no port suffix (e.g. "http://espoolbuddy-console.local"). Accepts
            # either a single string or a list; when multiple are given, the first is
            # authoritative (its heartbeat response is the only one honored for
            # tare/calibrate/write_tag commands, and it alone drives the connectivity
            # LED) while the rest are push-only observers. The port (CONSOLE_PUSH_PORT)
            # is applied automatically in the component. Setting it makes this
            # device a satellite: it never talks to Bambuddy itself.
            cv.Optional(CONF_CONSOLE_URL, default=[]): cv.ensure_list(cv.string),
            # Sleep / deep-idle (console only): seconds of UI inactivity before the
            # backlight is switched off and the backend cadence is reduced. 0 = off.
            cv.Optional(CONF_SLEEP_TIMEOUT, default=600): cv.positive_int,
            # Multiplier applied to the heartbeat and printer-poll intervals while
            # asleep (e.g. 6 turns a 10 s heartbeat into 60 s).
            cv.Optional(CONF_SLEEP_FACTOR, default=6): cv.int_range(min=1),
            # Optional hardware this device may not have — when omitted, the
            # component gracefully no-ops calls that would otherwise target it
            # and reports its absence honestly to the backend. (An NFC reader
            # is picked up automatically when a bambuddy_nfc: block exists.)
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(RtttlComponent),
            # Mandatory for any console device (see _validate_roles below) —
            # every display-having device has a real backlight; only headless
            # satellites (console_url set) are exempt.
            cv.Optional(CONF_BACKLIGHT_ID): cv.use_id(LightState),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_roles,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_backend_url(config[CONF_BACKEND_URL]))
    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_hostname(config[CONF_HOSTNAME]))
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_heartbeat_fail_threshold(config[CONF_HEARTBEAT_FAIL_THRESHOLD]))
    cg.add(var.set_scale_report_interval(config[CONF_SCALE_REPORT_INTERVAL]))
    cg.add(var.set_printer_poll_interval(config[CONF_PRINTER_POLL_INTERVAL]))
    scale = config[CONF_SCALE]
    if scale[CONF_TYPE] == SCALE_TYPE_REMOTE:
        cg.add(var.set_scale_source(ScaleSource.REMOTE))
        cg.add(var.set_remote_scale_timeout(scale[CONF_TIMEOUT].total_milliseconds))
    elif scale[CONF_TYPE] == SCALE_TYPE_LOCAL:
        cg.add_define("USE_BAMBUDDY_SCALE_SENSOR")
        cg.add(var.set_scale_source(ScaleSource.LOCAL))
        sens = await cg.get_variable(scale[CONF_SENSOR])
        cg.add(var.set_scale_sensor(sens))
        cg.add(var.set_local_scale_stable_after(scale[CONF_STABLE_AFTER].total_milliseconds))
        cg.add(var.set_local_scale_stable_band(scale[CONF_STABLE_BAND]))
        cg.add(var.set_local_scale_timeout(scale[CONF_TIMEOUT].total_milliseconds))
    for url in config[CONF_CONSOLE_URL]:
        cg.add(var.add_console_url(url))
    cg.add(var.set_sleep_timeout(config[CONF_SLEEP_TIMEOUT]))
    cg.add(var.set_sleep_factor(config[CONF_SLEEP_FACTOR]))
    if CONF_SPEAKER_ID in config:
        speaker = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_speaker_component(speaker))
    if CONF_BACKLIGHT_ID in config:
        backlight = await cg.get_variable(config[CONF_BACKLIGHT_ID])
        cg.add(var.set_backlight_component(backlight))
    if CORE.is_esp32:
        from esphome.components.esp32 import include_builtin_idf_component

        include_builtin_idf_component("esp_http_client")
        # bambuddy_api.cpp includes esp_tls.h directly; esp_http_client doesn't
        # reliably re-export it, so list the component rather than depend on
        # another component happening to pull it in.
        include_builtin_idf_component("esp-tls")
        # Every esp_http_client config here sets crt_bundle_attach, so the
        # component needs esp_crt_bundle.h — mbedtls only puts that header on
        # the include path when the Mozilla CA bundle is enabled. Scale devices
        # never speak TLS, but they compile the same code, so require it here
        # rather than leaving every device YAML to remember the option.
        _require_certificate_bundle()
        # Consoles receive satellite pushes over httpd.
        include_builtin_idf_component("esp_http_server")
        # A local load cell keeps its tare/calibration in NVS.
        if scale[CONF_TYPE] == SCALE_TYPE_LOCAL:
            include_builtin_idf_component("nvs_flash")
