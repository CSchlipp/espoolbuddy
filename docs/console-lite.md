← [Back to README](../README.md)

# Console build: Lite (Waveshare ESP32-S3-Touch-LCD-1.47)

A small, low-interaction console for a **1.47″ 172×320** screen. It shows
the scale weight, identifies the spool you put on the NFC reader, and
assigns it to the AMS slot you load it into, all without a single tap.
It shows the same screens as the full console, condensed onto one screen;
Quick Weight, adding an unlinked tag to the inventory, unlinking a tag and
the settings are each a tap or two.
Firmware: [`espoolbuddy_console_lite.yaml`](../espoolbuddy_console_lite.yaml).

<img src="images/ui/lite/nfc-spool.png" alt="Lite console: scale weight at the top, a scanned PLA Silk Gold spool below it with a countdown to insert it into the AMS, and a Quick Weight button" width="172"> 

*A scanned spool waiting to be loaded into the AMS.*

It talks to Bambuddy directly, like the other consoles, and has its own PN532
for scanning. It has no load cell: the weight comes from an
[ESPoolBuddy Scale](scale.md), which pushes its readings to every console in
its `console_url` list. Without a scale, the weight area just reads
*No scale*. Scanning and auto-assign work either way.

The Lite console runs its own compact UI
(`espoolbuddy/bundles/console_lite.yaml`), not the tabbed UI of the [WT32-SC01 Plus](console-wt32sc01.md) and
[Panda Touch](console-pandatouch.md) consoles. Use one of those if you want
to browse AMS slots, link tags to existing spools, or manage storage
locations on the device itself (see [Known limitations](#known-limitations)
and the [feature comparison](comparison.md#full-console-vs-lite)).

## Bill of materials

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-Touch-LCD-1.47 | ESP32-S3, 16 MB flash, 8 MB PSRAM, 172×320 touch display (JD9853 panel, AXS5106L touch). Display, touch and backlight are onboard. |
| PN532 NFC module | **I²C mode**: DIP switch **1 = ON, 2 = OFF** |
| 5 jumper wires | PN532 to the board's header |
| USB-C cable | For the first (wired) flash and power |
| [ESPoolBuddy Scale](scale.md) | Optional: provides the weight reading |

## Wiring the NFC reader

The PN532 runs in I²C mode on its own bus, separate from the touch
controller. The NFC driver polls from its own task, so sharing the touch bus
would let the two collide.

| PN532 pin | Board | Notes |
|---|---|---|
| GND | GND | |
| VCC | 3V3 | |
| SDA | GPIO17 | I²C data (`nfc_i2c` bus) |
| SCL | GPIO18 | I²C clock |
| IRQ | GPIO16 | Open-drain; firmware enables an internal pull-up |

With the IRQ wire the firmware reacts to a tag right away, as on the
WT32-SC01 Plus console. Without it, remove `irq_pin:` from the YAML and the
firmware falls back to polling every `poll_interval` (300 ms by default),
as on the Panda Touch. That still works, just slightly slower to react.

> **Pins to double-check.** The display data/control pins, display reset
> (GPIO40) and touch reset (GPIO47) are confirmed. These are best guesses,
> marked `# unverified` in the YAML:
> - backlight GPIO46
> - touch SDA/SCL GPIO42/41 and touch INT GPIO48
> - PN532 bus GPIO17/18 and IRQ GPIO16
> - the octal PSRAM mode
>
> If the backlight stays dark, touch doesn't respond, or boot reports a
> PSRAM error, check those first against Waveshare's schematic. Any free
> header pins work for the PN532 bus and IRQ.

<details>
<summary>Pin map (reference only)</summary>

| Function | GPIO |
|---|---|
| LCD SCK / MOSI / CS / DC / RST | GPIO38 / GPIO39 / GPIO21 / GPIO45 / GPIO40 |
| Backlight (PWM) | GPIO46 *(unverified)* |
| Touch (AXS5106L) SDA / SCL | GPIO42 / GPIO41 *(unverified)* |
| Touch INT / RST | GPIO48 *(unverified)* / GPIO47 |
| PN532 SDA / SCL | GPIO17 / GPIO18 *(unverified, any free pair)* |
| PN532 IRQ | GPIO16 *(unverified, any free pin)* |

</details>

The display is driven by `mipi_spi` with the JD9853 vendor init from
Waveshare's demo (40 MHz, 34 px column offset) in its native portrait orientation. The touch controller uses the community
[`axs5106`](https://github.com/widget/esphome-components) component, which
the YAML pulls in automatically.

## Get the weight from the scale

Add the Lite console to the scale's `console_url` list (in
`espoolbuddy_scale.yaml`); see [Scale → Point it at the console](scale.md#point-it-at-the-console):

```yaml
console_url:
  - "http://espoolbuddy-console.local"   # first entry: authoritative console
  - "http://espoolbuddy-lite.local"      # the Lite console
```

If the Lite console is the only one, list just its URL. Tare and calibrate
commands only reach the scale from the **first** console in the list, so put
the Lite console first if you want its **Tare** and **Calibrate** buttons to
work.

## Using it

The Lite shows the same screens as the full console's NFC and Scale tabs,
with the same texts, colours and icons, stacked on one screen. The header
works like the full console's:
- the printer icon is green while the selected printer is connected, red
  otherwise;
- the middle shows the clock (the selected printer is on the settings
  panel's Printer tile);
- status messages (for example "Weight saved", "Created spool #43",
  "Timed out — rescan to assign") take that place for 6 s.

### Weighing

| | |
|---|---|
| <img src="images/ui/lite/scale.png" alt="Weight 1023.4 g, Stable, an NFC icon and 'Place spool tag on reader'" width="172"> | **Stable.** The live scale weight; it reads *Stable* once the reading has settled. |
| <img src="images/ui/lite/scale-settling.png" alt="Weight 1019.8 g with 'Settling'" width="172"> | **Settling.** The reading is still moving; it reads *Stable* once it is steady. |
| <img src="images/ui/lite/scale-no-scale.png" alt="'No scale connected' and 'Place spool tag on reader'" width="172"> | **No scale connected.** No weight pushes are arriving. Scanning and auto-assign still work; Quick Weight is hidden. |

### Scanning and auto-assign

| | |
|---|---|
| <img src="images/ui/lite/nfc-waiting.png" alt="NFC icon and 'Place spool tag on reader' below the weight" width="172"> | **Waiting for a tag.** With nothing on the reader the screen shows *Place spool tag on reader* (the weight stays on top, so this is the same view as above). |
| <img src="images/ui/lite/nfc-spool.png" alt="Spool view: gold spool icon, eSUN PLA Silk, Gold, 455g (46%) with a progress bar, #39, a 59s countdown and the Quick Weight button" width="172"> | **Spool scanned.** As on the full console: spool icon in the filament colour, brand / material, colour, remaining filament with the level bar, the spool ID and the tag type. Load it into any AMS slot while the countdown (top right) runs and it is assigned automatically. |
| <img src="images/ui/lite/assigned.png" alt="Green banner 'Assigned: AMS-A · Slot 4' below the spool view" width="172"> | **Assigned.** The *Assigned:* banner names the slot; after 4 s the screen returns to *Place spool tag on reader*. |
| <img src="images/ui/lite/timeout.png" alt="'Place spool tag on reader' with 'Timed out — rescan to assign' in the header" width="172"> | **Timed out.** No AMS load within 60 s. Scan the tag again to retry. |
| <img src="images/ui/lite/ams-removed.png" alt="Spool view for a Bambu Lab PLA Matte Charcoal spool #42 with a 60s countdown" width="172"> | **Taken out of the AMS.** A known spool removed from an AMS slot comes up like a fresh scan, ready for Quick Weight. Put it back, or into another slot, and that slot is assigned. |
| <img src="images/ui/lite/location.png" alt="Spool view with a purple 'Shelf A' badge and 'Assigned to Shelf A' in the header" width="172"> | **Storage location.** With a spool on screen, scan a storage-location tag: the spool is assigned to that location in Bambuddy, and the location shows as a badge. Location tags are linked on a full console or in Bambuddy. |

### Quick Weight

| | |
|---|---|
| <img src="images/ui/lite/quick-weight.png" alt="Quick Weight button reading '773g (1023g)' in green and 'Weight saved' in the header" width="172"> | **Stable reading.** The button shows the filament weight and the total, e.g. *773g (1023g)*. One tap records it in Bambuddy (*Weight saved*). |
| <img src="images/ui/lite/weight-popup.png" alt="'Update Spool Weight' popup: PLA · Gold, current scale reading 1020 g, Update and Cancel" width="172"> | **Not stable yet.** The *Update Spool Weight* popup shows the live reading; **Update** records it. |
| <img src="images/ui/lite/archive.png" alt="'Spool Empty' popup: PLA Matte · Bambu Lab, Archive this spool?, Archive and Keep" width="172"> | **Empty spool.** If the weight says no filament is left, the Lite offers to archive the spool in Bambuddy. **Keep** leaves it as it is. |

### Unlinked tags

| | |
|---|---|
| <img src="images/ui/lite/nfc-unlinked.png" alt="'New generic NFC Tag discovered', 'No filament info on tag', UID, and an Add to Inventory button" width="172"> | **Unlinked tag.** The tag isn't linked to a spool yet. |
| <img src="images/ui/lite/nfc-unlinked-opentag3d.png" alt="'New OpenTag3D discovered' with an Ocean Blue spool icon, 'eSUN PETG CF · Ocean Blue · 1000 g', UID, and Add to Inventory" width="172"> | **OpenTag3D tag.** Its decoded data (manufacturer, material, colour, weight) is previewed the same way. |
| <img src="images/ui/lite/nfc-unlinked-bambu.png" alt="'New Bambu Lab Tag discovered' with a Desert Tan spool icon, 'PLA Matte · Desert Tan · 1000 g', UID, and Add to Inventory" width="172"> | **Tag with spool data.** Bambu Lab and OpenTag3D tags carry material and colour, shown as a preview. **Add to Inventory** creates a spool from that data (a generic 1000 g PLA for a plain tag) and links the tag to it. |
| <img src="images/ui/lite/created.png" alt="Spool view of the new Bambu Lab PLA Matte Desert Tan spool #43 with a 59s countdown and 'Created spool #43' in the header" width="172"> | **Created.** The new spool is shown like a scanned one, ready to be loaded into the AMS or weighed. |

### Spool actions

Tap the spool view (the ⋮ in its corner) for the same actions as the full
console's ⋮ menu, plus Discard.

| | |
|---|---|
| <img src="images/ui/lite/spool-actions.png" alt="Overwrite Tag, Print Label, Unlink Tag, Discard and Cancel" width="172"> | **Write Tag** has Bambuddy write the spool's data to the NTAG on the reader; it reads **Overwrite Tag**, with a confirmation, when the tag already holds data. **Print Label** prints the spool's label ([label printer](label-printer.md) only). **Unlink Tag** removes the tag from the spool. **Discard** clears the spool from the screen. |
| <img src="images/ui/lite/unlink-confirm.png" alt="'Are you sure? Unlink this tag from its spool?' with Cancel and Unlink" width="172"> | **Unlink confirm.** Afterwards the screen shows *Place spool tag on reader* with *Tag unlinked*. Scan the tag again and it shows as unlinked, ready for **Add to Inventory**. |
| <img src="images/ui/lite/unlinked-actions.png" alt="Discard and Cancel over an unlinked tag" width="172"> | **Unlinked tag actions.** **Discard** hides the tag until it is scanned again. |

### Build plate clear

| | |
|---|---|
| <img src="images/ui/lite/plate-clear.png" alt="Green 'Build Plate Clear' popup: Please confirm that the build plate has been cleared, with Discard and Confirm" width="172"> | When Bambuddy waits for the build plate to be cleared (its "Require plate-clear confirmation" setting), the popup appears and wakes the screen. **Discard** hides it until the printer asks again. |

### Settings

Tap the cog, or swipe down, as for the full console's quick settings. Swipe
up or tap ✕ to close. The panel scrolls.

| | |
|---|---|
| <img src="images/ui/lite/settings.png" alt="Settings: Printer tile (X1C Workshop, 1 / 3), Power tile (On), Tare and Calibrate, brightness and inactivity rows" width="172"> | **Printer** — tap to switch to the next printer; auto-assign uses the selected one. **Power** — tap to switch the printer's power plug (the plug Bambuddy marks as controlling the printer; the tile is hidden without one). **Tare** and **Calibrate** the scale. **Brightness** (−/+). |
| <img src="images/ui/lite/settings-more.png" alt="Settings scrolled: inactivity 10 min, NFC in sleep Slow, Clear Location on AMS Load On, connection icons and version" width="172"> | **Inactivity** (−/+) — sleep after 5–60 min without touch, scan or weight change, or *Never*. **NFC in sleep** — tap to switch between *Slow* (default) and *On*, which keep scanning so a tag wakes the screen, and *Off*, which stops the reader while asleep. **Clear Location on AMS Load** — tap for On/Off. WiFi, Bambuddy and scale status, and the firmware version. |
| <img src="images/ui/lite/calibrate.png" alt="'Calibrate Scale': 100 g with -1/+1, -10/+10, -100/+100, Cancel and Calibrate" width="172"> | **Calibrate Scale.** Put a known weight on the empty scale, set it with the buttons, tap **Calibrate**. |

All settings are kept across restarts.

**Dim and sleep.** As on the full console, the backlight dims after 60 s
without activity and turns off after the *Inactivity* time. Putting a spool
on the scale, scanning a tag (unless NFC in sleep is *Off*), a build-plate
popup or a touch wakes it. The waking touch doesn't also press a button.

## Known limitations

The Lite covers the day-to-day loop. Where it differs from the full console,
it's because the 1.47″ screen is too small:

- **Spool actions are behind a tap** on the spool view instead of a row of
  buttons.
- **The header shows only the clock**, not the full console's
  "printer - clock"; the selected printer is on the settings panel.
- **Settings are one scrolling panel** of compact rows (−/+ steppers and
  tap-to-switch values) instead of the full console's settings pages with
  their slider and segment buttons.
- **Text is heavier and brighter.** The Lite uses Roboto Medium (14 px for
  the smallest text instead of 12 px) and lighter greys for secondary text,
  because the full console's thin Roboto Regular is hard to read on the
  small panel.
- **Linking a tag to an existing spool** (the full console's *Assign Spool*
  picker) isn't available. The Lite can add a new spool from an unlinked
  tag.
- **No AMS overview, slot details or manual slot assignment.**
- **No linking of tags to storage locations, and no smart-plug list.**
  Scanning an already linked location tag, and the printer's power plug,
  do work.
- **No speaker**, so no chimes, and no *WiFi in sleep* setting.
- **Tare and calibrate need the first `console_url` entry.** A scale only
  takes these commands from the first console in its list
  ([above](#get-the-weight-from-the-scale)).

## Setup & flashing

Wire the PN532 as above, then follow the
[setup & flashing guide](setup.md), using `espoolbuddy_console_lite.yaml`
wherever it says `<console file>`.

To try the UI without hardware, run the
[desktop simulator](development.md):
`esphome run host/espoolbuddy_console_lite_host.yaml`.
