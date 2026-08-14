← [Back to README](../README.md)

# Console build: BigTreeTech Panda Touch

A second, ready-to-flash console build for the
**[BigTreeTech Panda Touch](https://github.com/bigtreetech/docs/blob/master/docs/PandaTouch.md)**
— a bigger 5″ screen on a single self-contained board, nothing to wire.
Firmware: [`espoolbuddy_console_pandatouch.yaml`](../espoolbuddy_console_pandatouch.yaml).

It runs the exact same UI as the [WT32-SC01 Plus console](console-wt32sc01.md)
(the layout adapts to the screen size automatically) — just missing NFC and
a speaker, since the board doesn't have either.

## Bill of materials

| Part | Notes |
|---|---|
| [BigTreeTech Panda Touch](https://github.com/bigtreetech/docs/blob/master/docs/PandaTouch.md) | ESP32-S3, 800×480 touchscreen, 8 MB PSRAM, 16 MB flash. Display, touch and backlight are all onboard — nothing extra to wire. |
| USB-C cable | For the first (wired) flash and power |

## Known limitations

- **No built-in NFC.** There's no PN532 on this board. If you want NFC
  scanning with this console, add a [Scale](scale.md) — it has its own
  PN532 and pushes scan events to the console over the network, so tags
  still reach Bambuddy, just via the scale instead of the console.
- **No speaker.** The tag-scan/weight-stable chimes are silently skipped —
  the Settings screen hides the Sound toggle automatically since there's no
  speaker to control.
- **Occasional display glitches.** This build has shown intermittent visual
  artifacts on real hardware. It's believed to be related to how this
  board's RGB-parallel display works, rather than a firmware bug — the
  WT32-SC01 Plus console doesn't have this issue. Not yet fully solved.

## Setup & flashing

No wiring needed — just follow the [setup & flashing guide](setup.md), using
`espoolbuddy_console_pandatouch.yaml` wherever it says `<console file>`.
