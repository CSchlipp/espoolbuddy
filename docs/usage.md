← [Back to README](../README.md)

# Using the device

- **Scan a tag**: hold an NFC tag (Bambu Lab spool tag, or your own NTAG
  213/215/216) near the console's or scale's PN532 antenna. The console
  jumps to the NFC tab automatically on a new scan.
- **Scan + load auto-assigns the AMS slot**: after scanning a tag linked to
  a known spool, loading that physical spool into any AMS slot within the
  next 60 s automatically assigns the spool to that slot in Bambuddy and
  configures it — no manual assignment step needed.
- **Removing a spool from the AMS behaves like scanning it**: when a known,
  already-loaded spool is taken out of an AMS slot, ESPoolBuddy treats that
  exactly like the tag being scanned again (same chime, same jump to the NFC
  tab). You can immediately load it into the same or a different slot and
  that slot gets assigned/configured automatically, same as a fresh scan.
- **Unknown tag → create a spool entry**: scanning a tag with no matching
  spool in Bambuddy opens the unlinked-tag panel with an **Add to
  Inventory** button. This creates a default spool entry (PLA, 1000 g)
  linked to that tag, tagged with a `"Created by ESPoolBuddy"` note so it's
  easy to identify later.
- **Unknown tag → link to an existing spool**: from that same panel, use
  **Assign Spool** instead to link the tag to an existing, untagged spool.
  The picker shows your 9 most recently added spools in a grid for quick
  selection, but you're not limited to those — a numeric field with ±1/10
  steppers lets you type/dial in any spool ID directly.
- **Weigh a spool**: place it on the scale — the weight and a "stable"
  indicator show on the console once the reading settles (~750 ms after it
  stops changing).
- **Write a tag**: trigger `write_tag` from the Bambuddy dashboard, then
  present a blank/writable NTAG to the console or scale within the timeout.
- **AMS / printer status**: the console's AMS tab shows live slot/filament
  state polled from Bambuddy, along with each AMS unit's temperature,
  humidity, and its custom name if one was set in Bambuddy (falls back to a
  default label otherwise). The currently selected printer's name also
  shows in the header next to the clock; it only scrolls if the text is too
  long to fit, otherwise it just sits centered.
- **AMS rows per screen**: Settings → Features → **Rows/Page** controls how
  many AMS rows are shown at once (1/2/3 on the WT32-SC01 Plus console,
  1/2/3/5 on Panda Touch). Fewer rows per page means each row — and its
  icons and text — grows to fill the freed space instead of leaving it
  blank; more AMS units than fit on one page are reached via the existing
  prev/next buttons. Defaults to the max for your console, which looks the
  same as before this setting existed.
- **Sleep**: the console dims its backlight after 60 s idle and can go into
  a deep-idle "sleep" tier (backlight off, reduced network cadence) after a
  configurable timeout — any touch, new tag, or weight change wakes it.
- **Scale status LED**: red = no WiFi, blue = WiFi up but console
  unreachable, green = fully connected.

See the [configuration reference](configuration.md) for the settings behind
several of these (poll intervals, sleep timeout, clock format, and more).
