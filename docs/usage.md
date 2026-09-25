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
- **Storage locations**: from Settings → **Storage Locations**
  you can link an NFC tag to any storage location already created in
  Bambuddy — tap **Scan to Link** on a location, a popup confirms it's
  waiting (tap **Dismiss** in it to back out), then present the tag. Tap
  **Unlink** to clear a location's tag. A tag already linked to another
  location — or already linked to a spool — is refused, with that same
  popup instead naming what it's linked to, rather than silently ending up
  claimed by two things at once; unlink it there first if you want to move
  it. Once linked, scanning that tag while a spool is staged (freshly
  scanned, or just removed from an AMS slot) assigns the spool to that
  location in Bambuddy; scanning it with nothing staged just shows a brief
  confirmation. A spool's current storage location, if any, shows as a
  purple badge next to its icon on the NFC detail view. By default, loading
  a spool into an AMS slot also clears its storage location automatically
  (Bambuddy itself does not do this) — turn this off in Settings → Features
  → **Clear Location on AMS Load** if you'd rather it was left as-is.
  With Spoolman, the location list comes from Bambuddy's own catalog, which
  it fills from the locations found on your Spoolman spools when it refreshes
  its spool list (at most about once a minute), so a location you only just
  typed into Spoolman can take a moment to show up here. A spool that was
  loaded into an AMS under Bambuddy before 1.2.5.4 may still carry an old
  location like `H2D-1 - AMS A1` in Spoolman, and the badge shows it as-is.
  Linking a tag to a location is an ESPoolBuddy feature: the tag is kept in
  the location's `identifier` field in Bambuddy, which Bambuddy itself does
  not use yet.
- **Unknown tag → create a spool entry**: scanning a tag with no matching
  spool in Bambuddy opens the unlinked-tag panel with an **Add to
  Inventory** button. For a Bambu Lab tag the new spool is populated from
  the tag itself — material and subtype, colour name and RGBA, brand,
  label weight, slicer filament id and hotend temperatures. For any other
  tag (NTAG, foreign spool, or a tag that could not be decoded) it falls
  back to a default entry (PLA, 1000 g). Either way the spool is linked to
  that tag and carries a `"Created by ESPoolBuddy"` note so it's easy to
  identify later. With the Spoolman backend everything above is still filled
  in except the hotend temperatures, which Bambuddy does not accept for
  Spoolman spools. This works the same whether the tag was scanned on the
  console or on the scale — the scale decodes the tag itself and pushes the
  decoded values to the console along with the UID.
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
- **Swipe between AMS pages**: on the AMS tab, swipe left for the next page and
  right for the previous one — the same thing the prev/next buttons do, and
  equally limited to the pages that actually exist.
- **AMS rows per screen**: Settings → Features → **Rows/Page** controls how
  many AMS rows are shown at once (1/2/3 on the WT32-SC01 Plus console,
  1/2/3/5 on Panda Touch). Fewer rows per page means each row — and its
  icons and text — grows to fill the freed space instead of leaving it
  blank; more AMS units than fit on one page are reached via the existing
  prev/next buttons. Defaults to the max for your console, which looks the
  same as before this setting existed.
- **Plate-clear popup**: when Bambuddy's queue is waiting for you to confirm
  the build plate is empty after a print, the console shows a full-screen
  **Build Plate Clear** popup (and wakes the screen) with **Confirm** and
  **Discard** buttons — Confirm does the same as Bambuddy's "Clear Plate"
  button. It follows Bambuddy's **Require plate-clear confirmation** setting
  (Settings → Workflow → Queue & Dispatch, off by default): on in Bambuddy 
  shows the popup after every print, off never shows it. The setting is 
  unrelated to Bambuddy's camera-based plate detection. The console re-reads
  it every poll interval, so a change in Bambuddy takes effect within one 
  poll interval (30 s by default).
- **Header clock format**: follows Bambuddy's **Time format** setting.
  **12h** shows `2:05 PM`; **24h** and **System** both show `14:05`, since
  the console has no system locale to follow. Like the plate-clear setting,
  it's re-read every poll interval.
- **Quick settings**: swipe down from anywhere on the console to pull open a
  quick-settings drawer, and swipe up, tap its chevron handle, or tap the
  dimmed area behind it to close it again. It holds up to three tiles:
  **Power** — only shown when there is a smart plug to act on: one assigned
  to the selected printer, or one not assigned to any printer. If the
  selected printer has a plug with "Controls printer power" enabled in
  Bambuddy, tap to toggle it on/off (icon lights up green while on) and hold
  to open a popup listing every relevant plug (the printer's own, plus any
  not assigned to a printer) so you can flip each individually. If there is
  no such power plug, tapping opens that popup directly. The popup stays
  open after a toggle so you can switch several in a row, and only closes
  via its **Close** button — **Printer** — tap to
  switch to the next printer, hold to open the full printer picker; the icon
  is green while that printer is reachable — and **Sound**, which
  mutes/unmutes the chimes and stays in sync with Settings → Features →
  Sound. The drawer stays out of the way while a dialog is open or the
  screen is asleep.
- **Sleep**: the console dims its backlight after 60 s idle and can go into
  a deep-idle "sleep" tier (backlight off, reduced network cadence) after a
  configurable timeout — any touch, new tag, or weight change wakes it.
- **Scale status LED**: red = no WiFi, blue = WiFi up but console
  unreachable, green = fully connected.

See the [configuration reference](configuration.md) for the settings behind
several of these (poll intervals, sleep timeout, and more).
