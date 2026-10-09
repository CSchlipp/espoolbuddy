← [Back to README](../README.md)

# Configuration reference

Edit the top of your device's entry-point YAML to change these —
[`espoolbuddy_console.yaml`](../espoolbuddy_console.yaml),
[`espoolbuddy_console_pandatouch.yaml`](../espoolbuddy_console_pandatouch.yaml),
or [`espoolbuddy_scale.yaml`](../espoolbuddy_scale.yaml). All three share the
same `bambuddy_api` component; several options only apply to one mode.

| Key | Applies to | Default | Description |
|---|---|---|---|
| `bambuddy_api.backend_url` | Console | *(secrets)* | Bambuddy server URL |
| `bambuddy_api.api_key` | Console | *(secrets)* | Bambuddy API key |
| `bambuddy_api.hostname` | Both | `SpoolBuddy-ESP` | Display name in the Bambuddy UI |
| `bambuddy_api.heartbeat_interval` | Console | `10` s | Heartbeat / command-poll frequency |
| `bambuddy_api.printer_poll_interval` | Console | `30` s | AMS/printer state poll frequency |
| `bambuddy_api.sleep_timeout` | Console | `600` s | Idle time before deep sleep (`0` = disabled); also adjustable live from the Settings tab |
| `bambuddy_api.sleep_factor` | Console | `6` | Heartbeat/poll interval multiplier while asleep |
| `bambuddy_api.console_url` | Scale | `http://espoolbuddy-console.local` | Console URL(s) to push readings to — a single string or a list, see [Scale build](scale.md#point-it-at-the-console). Setting it makes the device push to the console instead of talking to Bambuddy, so it can't be combined with `backend_url` |
| `bambuddy_api.scale` | Both | console: `type: remote` | Where weight comes from — see [below](#weight-source-scale) |
| `bambuddy_api.scale_report_interval` | Both | `1000` ms (scale YAML: `100`) | Scale: weight push cadence to the console. Console: minimum interval between weight reports to Bambuddy (a change of the stable flag is reported at once) |
| `bambuddy_nfc.interface` | Both | `spi` | PN532 host interface: `spi` or `i2c` — see [below](#nfc-reader-interface) |
| `bambuddy_nfc.poll_interval` | Both | `300` ms | Fallback polling rate (only used if IRQ isn't wired) |
| `bambuddy_nfc.miss_threshold` | Both | `3` | Missed reads before a "tag removed" event fires |

## Weight source (`scale:`)

Each device has exactly one weight source, picked by `type:`. Bambuddy keeps
one tare/calibration per device, so two sources on one device would overwrite
each other's calibration.

```yaml
bambuddy_api:
  scale:
    type: remote   # console default: a separate scale device pushes to it
    timeout: 10s   # no push for this long → "No scale connected"
```

```yaml
bambuddy_api:
  scale:
    type: local           # a load cell wired to this device
    sensor: spool_weight  # id of the load-cell sensor
    stable_after: 750ms   # reading counts as stable after this long without a move...
    stable_band: 0.3      # ...of at least this much (sensor units)
    timeout: 5s           # no reading from the sensor for this long → "No scale connected"
```

| `type` | Used on | Notes |
|---|---|---|
| `remote` | Console | Default on a console. Weight comes from a [scale device](scale.md) pushing to this console |
| `local` | Scale device, or a console with a [built-in load cell](console-builtin-scale.md) (optional, untested) | The component applies tare and calibration itself and keeps them in NVS |

A device with `console_url` and no `scale:` block only forwards NFC events.
`type: remote` isn't valid there: a device that pushes to a console can't
receive pushes from another scale.

**Sensor contract for `type: local`.** The sensor must report a value that is
linear in the load and **not tared**, with at least about one unit per gram:
raw ADC counts, or roughly scaled grams like the scale YAML's
`calibrate_linear` filter. Tare and calibration are applied on top as
`net = (value − tare) × factor`, so the sensor's own scaling only needs to be
consistent, not accurate. A `delta:` filter is fine, and recommended: liveness
is tracked from the sensor's unfiltered readings.

## NFC reader interface

`bambuddy_nfc` talks to the PN532 over SPI or I²C, chosen by `interface:`.
Both offer the same features (Bambu tag decoding, NTAG writes, NDEF
detection, NFC-in-sleep control). The reader connects itself to
`bambuddy_api` through its `api_id` — there is nothing to set on the
`bambuddy_api` side. Set the PN532 module's DIP switches/jumpers to the
matching mode.

| Option | `spi` | `i2c` | Notes |
|---|---|---|---|
| `spi_id` | ✅ | | The `spi:` bus (needs `mosi_pin` and `miso_pin`) |
| `cs_pin` | ✅ required | | Chip select |
| `data_rate` / `spi_mode` | ✅ | | Leave at `1MHz` / `0` — PN532 requirement |
| `i2c_id` | | ✅ | The `i2c:` bus (at most `400kHz`) |
| `address` | | ✅ | Default `0x24` |
| `irq_pin` | ✅ | ✅ | Optional; without it the status byte is polled over the bus |
| `poll_interval`, `miss_threshold` | ✅ | ✅ | See the table above |

The WT32-SC01 Plus console and the Scale use SPI with IRQ; the Panda Touch
uses I²C without IRQ (see [its wiring](console-pandatouch.md#wiring-the-nfc-reader)).

The `espoolbuddy_ref` substitution at the very top of each file is a
different kind of knob: it decides which version of this repo the build
pulls its components, UI packages and images from — see
[tracking `main` vs. pinning a release](setup.md#tracking-main-vs-pinning-a-release).

The Panda Touch console omits `speaker_id` entirely rather than setting it —
see its [known limitations](console-pandatouch.md#known-limitations) for what
that changes. Leaving out the `bambuddy_nfc:` block works the same way on any
console without a PN532: the NFC calls become no-ops and the NFC-in-sleep
setting hides itself.

For the lower-level HTTP endpoints and commands these settings feed into,
see the [API reference](api-reference.md).
