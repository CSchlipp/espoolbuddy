← [Back to README](../README.md)

# Built-in load cell on the console (optional, untested)

> **Untested.** This variant has not been built or run on real hardware yet.
> It has only been compile-checked. There is no ready-made case for it. The
> wiring below is a set of suggestions based on the boards' pinouts: check
> them against your board before soldering anything. If you build one,
> please report back
> ([#36](https://github.com/CSchlipp/espoolbuddy/issues/36)).

By default a console gets its weight from a separate [scale device](scale.md).
If you'd rather have a single box, you can wire an HX711 load-cell amplifier
straight to the console and let the console read it itself.

Trade-offs compared with the separate scale:

- **One weight source per device.** A console with a built-in load cell
  ignores weight pushed by a scale device. It still accepts NFC events from
  one, so a scale device can keep serving as an extra NFC reader.
- **It takes pins from somewhere.** Neither console board has two free
  GPIOs to spare, so you give up something: the USB-A port on the Panda
  Touch, the PN532's IRQ line on the WT32-SC01 Plus.
- **No case.** You'll need to design the mounting for the load cell yourself.

## Config

Add the HX711 sensor and point `bambuddy_api` at it. Replace the
`scale: type: remote` block in your console YAML with this one; the pins
depend on the board (see below):

```yaml
sensor:
  - platform: hx711
    id: spool_weight
    name: "Spool Weight"
    dout_pin: GPIOxx        # see the wiring for your board below
    clk_pin: GPIOyy
    gain: 128
    update_interval: 200ms
    filters:
      - median: {window_size: 5, send_every: 1}
      - sliding_window_moving_average: {window_size: 10, send_every: 2}
      - multiply: -1          # drop this if readings go negative under load
      - calibrate_linear: [0 -> 0, 1000000 -> 1000]   # rough scaling only
      - delta: 0.3

bambuddy_api:
  # ... existing options ...
  scale:
    type: local
    sensor: spool_weight
```

These filters are the same ones the [scale device](scale.md) uses. The sensor
only has to be linear and untared, with at least about one unit per gram. The
console applies tare and calibration itself and keeps them in NVS, so they
survive reboots and work without Bambuddy reachable. Tare and calibrate from
the **Scale** tab or from Bambuddy as usual. If the HX711 stops delivering
readings for 5 s, the console shows **No scale connected**. All options are in
the [configuration reference](configuration.md#weight-source-scale).

## Wiring: Panda Touch

The Panda Touch has no free header pins, but the USB-A port's data lines go
to the ESP32-S3's native USB pins: D− = GPIO19, D+ = GPIO20. Using them as
GPIO gives up the USB-A function. Logging and flashing go over USB-C (through
the CH340), so they're unaffected.

| HX711 pin | Panda Touch | Notes |
|---|---|---|
| DOUT | USB-A D− (GPIO19) | |
| SCK | USB-A D+ (GPIO20) | |
| VCC | 3.3 V, e.g. the rear I²C port's 3.3V pin (shared with the PN532) | **Not** the USB-A port's 5 V: DOUT would then drive 5 V into a 3.3 V GPIO |
| GND | GND (USB-A or the rear I²C port) | |

Things to check first:

- **Continuity.** Make sure D−/D+ actually reach GPIO19/20, and look for
  series resistors or an ESD/USB switch chip on the way. A plain cut-down
  USB-A plug is the least invasive way to connect.
- **Startup.** The USB-A port is often meant as a USB host port. Make sure
  nothing else on the board drives those lines at boot.

**NAU7802 instead of HX711?** Both I²C controllers are taken (touch on I2C0,
the PN532 on I2C1), so a NAU7802 would have to share the rear port's bus with
the PN532. Their addresses differ (`0x2A` vs `0x24`), so that should work
electrically, and it would leave the USB-A port alone. ESPHome has a
`nau7802` sensor platform, which needs the same `scale: type: local` setup
as above.

## Wiring: WT32-SC01 Plus

The expansion header has six IO pins. The PN532 already uses five of them
(SCK GPIO13, MOSI GPIO11, MISO GPIO12, CS GPIO10, IRQ GPIO14), which leaves
only **GPIO21**. The HX711 needs two pins, so the suggested trade is to give
up the PN532's IRQ line. NFC then runs in polling mode, exactly like on the
Panda Touch: tags are detected within about `poll_interval` (300 ms) instead
of instantly.

| HX711 pin | WT32-SC01 Plus | Notes |
|---|---|---|
| DOUT | GPIO14 (was PN532 IRQ) | Disconnect the PN532's IRQ wire |
| SCK | GPIO21 | Last free expansion-header pin |
| VCC | 3.3 V from the expansion header | Shared with the PN532 |
| GND | GND from the expansion header | |

Config changes on top of the block above:

- In `bambuddy_nfc:`, delete the `irq_pin:` block (polling takes over
  automatically).
- Use `dout_pin: GPIO14` and `clk_pin: GPIO21` for the HX711.

Things to check first:

- **Header pinout.** Confirm on your board that the sixth expansion IO is
  GPIO21. Revisions and clones differ.
- **Space.** The handheld case's battery compartment, or the SpoolEase case,
  leave little room for the HX711 board and load-cell wiring. Plan the
  mounting before cutting cables.
- **Keep the speaker.** The speaker's I2S pins (GPIO35–37) are internal and
  not affected.

## Why the separate scale stays the default

Bambuddy keeps a single tare and calibration per device. The separate scale
already stores its own, and the console relays them. A built-in load cell does
the same job inside the console, so you get one or the other, never both at
once. The separate scale is the tested, documented path with a ready-made
case. Use this page only if you specifically want a single box.
