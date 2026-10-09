← [Back to README](../README.md)

# ESPoolBuddy vs. SpoolBuddy

ESPoolBuddy is a from-scratch reimplementation of Bambuddy's official
[SpoolBuddy](https://wiki.bambuddy.cool/spoolbuddy/) client. Both talk to the
same Bambuddy backend API and cover the same basic job: scan a spool's tag,
weigh it, and keep the Bambuddy inventory in sync. They get there on very
different hardware, and each is the better choice for some setups. This page
tries to lay that out fairly. Where something is a trade-off rather than a
clear win, it says so.

> SpoolBuddy details on this page come from the
> [Bambuddy wiki](https://wiki.bambuddy.cool/spoolbuddy/) as of September 2026.
> SpoolBuddy is under active development, so check there for the current
> state.

## At a glance

| | **SpoolBuddy** (official) | **ESPoolBuddy** (this project) |
|---|---|---|
| Maintained by | The Bambuddy author, as part of Bambuddy | An independent community project |
| Compute | Raspberry Pi 4B / Pi 5 running Raspberry Pi OS | ESP32-S3 microcontroller running ESPHome firmware |
| Screen | 7″ HDMI, 1024×600 | 3.5″ 480×320 ([WT32-SC01 Plus](console-wt32sc01.md)) or 5″ 800×480 ([Panda Touch](console-pandatouch.md)) |
| UI | Bambuddy's own web UI in a browser kiosk | Native LVGL UI, reimplemented in firmware |
| NFC reader | PN5180 (SPI) | PN532 (SPI with IRQ, or I²C) |
| Scale | Built in: NAU7802 ADC + load cell under the device | Separate, optional device: HX711 ADC + load cell on its own ESP32-S3. Opt-in and untested: an HX711 wired [into the console](console-builtin-scale.md), no ready-made case |
| Form factor | One unit: screen, reader and scale together | Usually two units: a console, plus an optional scale that can sit elsewhere |
| Print control (pause / resume / progress) | No, the kiosk only shows AMS, inventory and spool screens | No, only AMS and spool screens |
| Can host Bambuddy itself | Yes ("full local mode" on the same Pi) | No, needs Bambuddy running somewhere else |
| Storage / OS upkeep | microSD card, Linux OS updates | None; firmware lives in on-board flash |
| Boot time | A Linux boot plus the browser starting | A few seconds |
| Power off | Needs a proper shutdown to protect the SD card | Pull the plug at any time |
| Battery / handheld use | Not designed for it | Supported on the WT32-SC01 Plus ([handheld case](console-wt32sc01.md#3d-printed-case)) |
| Updates | Installer script; Bambuddy can update it remotely over SSH | ESPHome OTA, from the ESPHome dashboard or Home Assistant |
| Approx. hardware cost (screen + reader + scale)¹ | ~120–210 USD | ~40–70 USD with WT32-SC01 Plus<br>~85–125 USD with Panda Touch |

¹ Rough prices as of September 2026, excluding shipping, taxes, printed
parts and tools. Actual prices vary by region and seller. The SpoolBuddy
figure is the Bambuddy wiki's own [estimate](https://wiki.bambuddy.cool/spoolbuddy/materials/).

## Features

Both cover the core workflow:

- Identify Bambu Lab RFID spools (MIFARE Classic) and NTAG213/215/216 tags.
- Write spool data to NTAG stickers.
- Show live spool weight and push it to the Bambuddy inventory.
- Toggle smart plugs and restart the device from a swipe-down quick menu.
- Work entirely on your LAN, with no cloud account.

### Where SpoolBuddy is ahead

- **It's the official client.** SpoolBuddy is built by the same person who
  builds Bambuddy, and its UI *is* Bambuddy's web frontend. When Bambuddy gains
  a feature, SpoolBuddy usually gets it straight away. ESPoolBuddy has to
  reimplement each feature in firmware, so it can lag behind. A backend API
  change can also break it until it's patched here.
- **Bigger screen.** 7″ at 1024×600 is noticeably roomier than either
  ESPoolBuddy console, especially the 3.5″ WT32-SC01 Plus.
- **One box, optionally including the server.** Reader, scale and screen sit
  in a single unit. If you don't already run Bambuddy, the same Pi can host it,
  so you don't need a separate server at all.
- **Remote management from Bambuddy.** Bambuddy can push updates to a
  SpoolBuddy over SSH and run real NFC/scale diagnostics on it. ESPoolBuddy
  can't accept SSH, and it answers the diagnostic commands with placeholder
  results (see the [API reference](api-reference.md#backend-commands-handled)).
  You update it through ESPHome instead.
-
- **Settings changes without reflashing.** Backend URL and API key can be
  changed on a running Pi. On ESPoolBuddy these live in `secrets.yaml`, and a
  change from Bambuddy only lasts until the next reboot, unless you reflash.

### Where ESPoolBuddy is ahead

- **Cost.** About a third of the price for a comparable console + scale setup
  (see the [table above](#at-a-glance)).
- **Nothing to maintain.** No SD card to wear out or corrupt, no Linux image
  to keep patched, no browser kiosk to babysit. It boots in seconds and
  survives being unplugged mid-use.
- **Low power, portable.** An ESP32-S3 with a small display draws around a
  watt, versus several watts for a Pi plus a 7″ HDMI panel. That's what makes
  the battery-powered handheld build practical.
- **Flexible layout.** The scale is a separate box, so it can sit on the shelf
  while the console lives next to the printer. You can run a console with no
  scale at all, or feed [one scale into several consoles](scale.md#point-it-at-the-console).
  If you'd rather have one box, a console can also read a
  [load cell wired to it](console-builtin-scale.md) (untested).
- **Fits into ESPHome / Home Assistant.** If you already run ESPHome, the
  devices show up in your existing dashboard and get OTA updates like
  everything else.
- **Reuses existing hardware and cases.** The WT32-SC01 Plus console and the
  scale use the same hardware as [SpoolEase](https://github.com/yanshay/SpoolEase),
  so existing SpoolEase builds and case designs can be reflashed. A Panda
  Touch that already sits next to a Bambu printer can be repurposed too.
- **Extra workflow features.** A few features aren't in Bambuddy itself:
  linking NFC tags to storage locations, clearing a spool's location
  automatically when it's loaded into the AMS, and treating a spool pulled
  from the AMS like a fresh scan. See [Using the device](usage.md) for
  details.



## Which one should I pick?

**Pick SpoolBuddy if you:**

- want the official, first-party client that tracks Bambuddy's features
  most closely;
- want a big screen and a single all-in-one unit;
- don't have a Bambuddy server yet and want one box to run everything;
- already have a Raspberry Pi and a touchscreen lying around.

**Pick ESPoolBuddy if you:**

- want the cheapest reasonable way to get an NFC console and scale for
  Bambuddy;
- don't want to maintain a Linux box, SD card and browser kiosk;
- want a battery-powered handheld, or a scale that lives apart from the
  screen;
- already run ESPHome / Home Assistant, or already own SpoolEase hardware
  or a Panda Touch.

Both talk to the same backend, and your spool inventory lives in Bambuddy,
not on the device. So this isn't a permanent choice: switching later doesn't
cost you any data.
