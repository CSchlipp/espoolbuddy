← [Back to README](../README.md)

# Troubleshooting

- **Scale never turns green (console unreachable)**: confirm `console_url`
  in `espoolbuddy_scale.yaml` matches the console's `esphome.name`, and that
  your router/network allows mDNS (`.local`) resolution between the two
  devices. As a fallback, replace the hostname with the console's static
  IP, e.g. `http://192.168.1.50`.
- **Console shows connection errors right after a reboot**: normal for the
  first ~10–30 s while WiFi/DNS converge — heartbeats retry automatically
  and it recovers on its own.
- **NFC not detecting tags**: double-check the PN532 module is jumpered for
  **SPI mode** (not I²C/UART — most modules default to I²C), and that
  IRQ is wired if you want instant detection instead of ~300 ms polling.
- **Scale reads negative or never settles**: verify the load cell's 4-wire
  connections; if the resting reading is negative, the polarity is handled
  in firmware (`multiply: -1` filter) — if it's still inverted, your load
  cell's wire colors differ from the assumed convention, remove/flip that
  filter in `espoolbuddy_scale.yaml`.
- **Config won't compile locally**: make sure you have a recent ESPHome
  (`pip install -U esphome`) — this project uses `mipi_spi`/LVGL features
  from modern ESPHome releases.
- **Printer picker only shows some of my printers**: the printer-selection
  popup (Settings tab) is backed by a fixed 25 static LVGL widget slots, not
  a true dynamic list — ESPHome's LVGL YAML can't create widgets at runtime,
  so the console pre-declares a ceiling and hides/populates however many are
  actually needed. Printers beyond the 25th in Bambuddy's list won't appear.
- **Panda Touch console shows visual glitches**: a known, not-yet-solved
  issue with this build — see its
  [known limitations](console-pandatouch.md#known-limitations).
