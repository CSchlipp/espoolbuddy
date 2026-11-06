← [Back to README](../README.md)

# Label printer (optional)

A console with PSRAM (WT32-SC01 Plus or Panda Touch) can print spool labels on
a nearby **Phomemo M220** over Bluetooth. A **Print Label** button appears on
the spool screen and on the card you get by tapping an AMS slot. Bambuddy
draws the label, the same one its label dialog prints (40×30 mm by default,
the M220's usual stock), with a QR code that opens the spool in Bambuddy. The
console only turns it into the printer's dots and sends it.

It's for the full console UI. The Lite console has no spool screen or slot
card to put the button on.

## Adding it

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

## Settings

All of them are substitutions:

| Substitution | Default | What it does |
|---|---|---|
| `label_printer_mac` | `00:00:00:00:00:00` | The printer's Bluetooth address; the default finds it on the first print |
| `label_template` | `box_40x30` | Bambuddy's label template: `box_40x30`, or `box_62x29` for 62 mm stock |
| `label_x_offset` | `right` | Where the roll runs under the 72 mm head: `right` (the M220 holder keeps a narrow roll against its right wall), `left`, `center`, or dots right of centre (8 per mm, negative for left) |
| `label_fields` | empty | The lines to print, comma-separated; empty is Bambuddy's default set |

Bambuddy's label fields, in the order it prints them: `brand`, `material`,
`hex`, `name`, `location`, `material_number`, `temps`, `weight`, `note`,
`added`, `qr`, `spool_id`. A line is left out when the spool has nothing for
it (no temperatures stored in Bambuddy, for example). Bambuddy sets the sizes:
the spool ID is the largest text and the QR code is at least 12 mm across.

## What it costs

Bluetooth stays off except while a label prints, because the Bluetooth stack
takes about 58 KB of the console's internal RAM while it runs. It's switched
on when you tap Print Label and off half a second after the label is out. The
package adds a little over 400 KB of firmware, nearly all of it the Bluetooth
stack, and about 32 KB of static RAM.

A print takes about 10 seconds from the tap: about 1 second for Bambuddy to
draw the label while the console connects, then 4 to 6 seconds to send it
and 2 to 3 for the printer to report it's done.

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
| Printer not found | No printer answered within 20 seconds: is it on and in range? |
| Printer disconnected | The connection dropped mid-label |
| Printing failed | The printer stopped taking data |

## Credits

The M220's commands and its "printed" reply follow the Phomemo driver in
[SpoolmanScale](https://github.com/Niko11111/SpoolmanScale) by Niko11111,
itself informed by the MIT-licensed myphomemo project. The code here is new.
