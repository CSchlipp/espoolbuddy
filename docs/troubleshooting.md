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
- **Bambu reads fail intermittently ("Bambu read failed for block N")**: a
  Bambu read is a long RF exchange — one authentication plus several block
  reads per sector — and a single dropout aborts the series even though the
  spool is still on the reader. The firmware retries the whole series up to
  four times while the same tag stays present (look for `Bambu read
  succeeded on attempt 2/4`), and treats the temperature and tray-UID blocks
  as optional, so losing one of those costs a detail rather than the scan.
  If it still fails repeatedly, the antenna is too far from the tag or the
  PN532's power supply is sagging.
- **Every spool of the same type lands on the same inventory entry**: fixed
  in 0.27.0. The tray UID was previously derived from blocks 4+5, which
  hold the material name and colour — identical on every spool of a given
  type, so they all collapsed onto one entry. The real per-spool identity
  lives in block 9. Note that the official SpoolBuddy client has the same
  defect (`BAMBU_BLOCKS = [1, 2, 4, 5]` in `daemon/pn5180.py`, combined in
  `nfc_reader.py` as `raw = blocks[4] + blocks[5]`), so entries created by
  it carry the same wrong identity. Inventory entries created before the fix
  keep their old identity — clear the stored tag link on those spools and
  they re-link correctly on the next scan. There is no fallback when block 9
  cannot be read: the tag then reports no tray UID at all and is identified
  by its own (unique) card UID, because a colliding identity silently merges
  distinct spools while an empty one only costs the link between a spool's
  two tags.
- **One scan produced several inventory entries**: fixed in 0.28.0. Every
  `CLICKED` event from the touch panel queued its own create, so a bouncing
  press — or a second press while the first POST was still in flight — added
  two or three entries for the same spool, stamped within the same second.
  The extras often had an empty `tray_uuid`, because the payload was re-read
  when the job ran rather than captured when the button was pressed, and a
  re-scan in between could lose it. "Add to Inventory" is now one-shot per
  physical scan (re-armed by the next scan, or immediately if the create
  failed), and the tag payload is snapshotted at the press.
- **Both tags of a spool must be scanned**: they don't. A Bambu spool
  carries two tags with different card UIDs but the same block-9 tray UID,
  so either side identifies the same spool.
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
