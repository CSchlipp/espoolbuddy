← [Back to README](../README.md)

# Label printer (optional)

A console with PSRAM (WT32-SC01 Plus, Panda Touch or the Lite) can print
spool labels on a **Phomemo M220** over Bluetooth, or on a **Brother QL** on
the network. **Print Label** appears in the spool screen's ⋮ menu, and on the
full console UI also on the card you get by tapping an AMS slot. Bambuddy
draws the label, the same one its label dialog prints, with a QR code that
opens the spool in Bambuddy. The console only turns it into the printer's dots
and sends it.

A console has one label printer: add one of the packages below, not both.

- [Phomemo M220 over Bluetooth](#phomemo-m220-over-bluetooth)
- [Brother QL on the network](#brother-ql-on-the-network)

## Phomemo M220 over Bluetooth

The label is 40×30 mm by default, the M220's usual stock.

### Adding it

Add the package next to the main one in your console's YAML:

```yaml
packages:
  # ... the espoolbuddy package, as before ...
  label_printer:
    url: https://github.com/CSchlipp/espoolbuddy.git
    ref: ${espoolbuddy_ref}
    path: espoolbuddy
    files: [label_printer_m220.yaml]
    refresh: always
```

Switch the printer on and tap **Print Label**. The first print finds the
printer by itself (it takes about a second) and the console remembers it, so
later prints connect straight away. If the remembered printer doesn't answer,
that print fails with "Printer not found" and the next one looks again, which
also covers a replaced printer.

With more than one M220 in range, give the console the one to use. Its
Bluetooth address is shown by a phone's BLE scanner app:

```yaml
substitutions:
  label_printer_mac: "55:E5:F4:95:C0:BC"
```

### What it costs

Bluetooth stays off except while a label prints, because the Bluetooth stack
takes about 58 KB of the console's internal RAM while it runs. It's switched
on when you tap Print Label and off half a second after the label is out. The
package adds a little over 400 KB of firmware, nearly all of it the Bluetooth
stack, and about 32 KB of static RAM.

A print takes about 10 seconds from the tap: about 1 second for Bambuddy to
draw the label while the console connects, then 4 to 6 seconds to send it
and 2 to 3 for the printer to report it's done.

## Brother QL on the network

The 300 dpi QL models with a network connection: QL-710W, QL-720NW, QL-810W,
QL-820NWB, or a QL-580N, with a **continuous** DK roll (DK-22205, 62 mm, is the
usual one). The console sends the label straight to the printer's raw
printing port (TCP 9100), and the printer cuts it off. Die-cut labels and the
wide QL-1100 series aren't supported.

### Adding it

Give the printer a fixed address (a DHCP reservation in your router, or a
static IP in its web page) and add the package next to the main one:

```yaml
substitutions:
  label_printer_host: "192.168.1.50"   # or its hostname

packages:
  # ... the espoolbuddy package, as before ...
  label_printer:
    url: https://github.com/CSchlipp/espoolbuddy.git
    ref: ${espoolbuddy_ref}
    path: espoolbuddy
    files: [label_printer_brother_ql.yaml]
    refresh: always
```

`label_printer_host` has no default: without it the config stops with "No
printer address: set the label_printer_host substitution ...". Load the roll
the label is made for, 62 mm unless you set `label_media` (see
[Settings](#settings)).

### What it costs

No Bluetooth, so little: about 20 KB of firmware and 1.5 KB of static RAM
(measured on `espoolbuddy_console.yaml`), and the label's image (about 30 KB
for a 62 mm label) only while it prints. The connection is opened for a print
and closed after it.

## Settings

All settings are substitutions: set the ones you want to change in your
console's YAML, next to the package. A setting a printer doesn't have is
marked —.

| Substitution | Phomemo M220 | Brother QL | What it does |
|---|---|---|---|
| `label_printer_mac` | `00:00:00:00:00:00` | — | The printer's Bluetooth address. The default finds the printer on the first print and remembers it; set it when there's more than one M220 in range |
| `label_printer_host` | — | **required** | The printer's IPv4 address or hostname. Give the printer a fixed address |
| `label_printer_port` | — | `9100` | The printer's raw printing port; leave it unless you've changed it on the printer |
| `label_media` | — | `62` | The continuous roll's width in mm: `12`, `29`, `38`, `50`, `54` or `62`. It must match the roll loaded, or the printer refuses the label ("Printer error"; the log says which roll it has). The M220 has none: it always prints across its whole 72 mm head, and `label_x_offset` puts the label on the roll |
| `label_cut` | — | `true` | Cut each label off. `false` leaves them on the roll (the M220 has a tear bar instead) |
| `label_template` | `box_40x30` | `box_62x29` | Which Bambuddy label to print: its size and layout (see [Templates](#templates)) |
| `label_x_offset` | `right` | `center` | Where the label sits across the head: `center`, `left`, `right`, or a number of dots right of centre (negative for left). The M220's holder keeps a narrow roll against its right wall; a Brother roll is centred by its guide. A dot is 1/8 mm on the M220 (203 dpi) and 1/11.8 mm on the Brother QL (300 dpi) |
| `label_fields` | empty | empty | The lines to print, comma-separated, e.g. `"spool_id,brand,material,qr"`. Empty is Bambuddy's default set |

### Templates

Bambuddy draws the label. Its label renderer has these templates; the
console can print the two single labels for small label printers:

| Template | Size (mm) | Phomemo M220 | Brother QL |
|---|---|---|---|
| `box_40x30` | 40 × 30 | ✓ (its usual stock) | ✓ on a 50, 54 or 62 mm roll; shrunk to 86% on 38 mm |
| `box_62x29` | 62 × 29 | ✓ on 62 mm or wider stock | ✓ on a 62 mm roll, shrunk to 95% (about 59 × 27.5 mm) |
| `ams_holder_74x33`, `ams_holder_75x55` | 74 × 33, 75 × 55 | — | — |
| `avery_5160`, `avery_l7160` | sheets for office printers | — | — |

The width is across the head, the length along the feed. A roll's printable
width is a little less than the roll (58.9 mm of a 62 mm Brother roll), so a
label up to 15% wider is shrunk to fit; a wider one is refused ("Label didn't
fit"), as is `box_40x30` on a 12 or 29 mm roll. The AMS holder labels are
wider than either printer's head, and the Avery templates are sheets, so the
console doesn't offer them.

To see a template before choosing it, open a spool in Bambuddy and use its
**Print label** dialog: it previews each template with your spool's data. The
templates are defined in Bambuddy's
[`backend/app/services/label_renderer.py`](https://github.com/maziggy/bambuddy/blob/main/backend/app/services/label_renderer.py).

### Fields

Bambuddy's label fields, in the order it prints them: `brand`, `material`,
`hex`, `name`, `location`, `material_number`, `temps`, `weight`, `note`,
`added`, `qr`, `spool_id`. A line is left out when the spool has nothing for
it (no temperatures stored in Bambuddy, for example). Bambuddy sets the sizes:
the spool ID is the largest text and the QR code is at least 12 mm across.

## What the button says

| Text | Meaning |
|---|---|
| Preparing label | Bambuddy is drawing the label, or the console is decoding it |
| Looking for printer | The console doesn't know the printer yet and is searching |
| Connecting to printer | Connecting to the remembered printer |
| Printing | Sending the label, or waiting for the printer to finish |
| Printed | The printer reported the label is out |
| Sent to printer | Everything was sent, but the printer didn't confirm |
| No label from Bambuddy | Bambuddy didn't send a label (check the backend is reachable) |
| Label didn't fit | The label is wider or longer than the printer takes |
| Printer not found | No printer answered within 20 seconds: is it on, and in range (Bluetooth) or on the network at its address? |
| Printer disconnected | The connection dropped mid-label, or the printer was switched off |
| Printer cover open | The printer reported its cover is open |
| Out of labels | The printer reported it has no labels, or couldn't feed them |
| Printer error | Anything else the printer reported, such as the wrong roll loaded |
| Printing failed | The printer stopped taking data |

## Credits

The M220's commands and its "printed" reply follow the Phomemo driver in
[SpoolmanScale](https://github.com/Niko11111/SpoolmanScale) by Niko11111,
itself informed by the MIT-licensed myphomemo project. The code here is new.

The Brother QL raster commands follow Brother's *QL Series Raster Command
Reference*, with roll margins as tabled in the
[brother_ql](https://github.com/pklaus/brother_ql) project.
