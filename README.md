# ESPoolBuddy

[![ESPHome Compile](https://github.com/CSchlipp/espoolbuddy/actions/workflows/esphome-compile.yml/badge.svg)](https://github.com/CSchlipp/espoolbuddy/actions/workflows/esphome-compile.yml)
[![GitHub stars](https://img.shields.io/github/stars/CSchlipp/espoolbuddy?style=social)](https://github.com/CSchlipp/espoolbuddy/stargazers)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-ffdd00?logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/cschlipp)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-support-ff5e5b?logo=ko-fi&logoColor=white)](https://ko-fi.com/cschlipp)

**ESPoolBuddy** turns a couple of cheap ESP32-S3 boards into an NFC-tagged
filament spool tracker for [Bambuddy](https://github.com/maziggy/bambuddy) —
scan a spool's tag, load it into your AMS, and it's identified and assigned
automatically. No Raspberry Pi, no SD card, no Linux image to maintain —
just [ESPHome](https://esphome.io) firmware flashed straight onto the
boards.

It's a from-scratch reimplementation of Bambuddy's official
[SpoolBuddy](https://github.com/maziggy/bambuddy/tree/main/spoolbuddy)
client (which normally runs on a Raspberry Pi) — same idea, same backend
API, running natively on ESP32 instead.

---

## How it fits together

![Architecture: this repo builds firmware for both the Scale and the Console, which talk to an external Bambuddy backend and printer](docs/images/architecture-overview.svg)

A full setup is two devices, and you only need the first one to get started:

- **Console** — a touchscreen that shows AMS/filament status, lets you scan,
  assign, and browse spools, and optionally shows live scale weight. It's
  the only device that talks to Bambuddy directly.
- **Scale** *(optional)* — a load cell + NFC reader that weighs a spool and
  pushes the reading straight to the console, which relays it to
  Bambuddy. It never talks to Bambuddy itself, so it just needs to know
  where the console is on your network.

Bambuddy and your Bambu Lab printer are separate, existing projects this
firmware talks to over the network — you'll need a
[Bambuddy](https://github.com/maziggy/bambuddy) instance running first.

---

## Pick your console

| | [WT32-SC01 Plus](docs/console-wt32sc01.md) ⭐ recommended | [Panda Touch](docs/console-pandatouch.md) |
|---|---|---|
| Screen | 3.5″, 480×320 | 5″, 800×480 |
| NFC built in | ✅ | ❌ — use the [Scale](docs/scale.md) instead |
| Speaker built in | ✅ | ❌ |
| Why pick it | Full feature set, smaller footprint | Bigger screen, one self-contained board, less to wire |

Both run the identical UI and firmware logic — only the hardware pinout
differs. Whichever you pick, you can add a [Scale](docs/scale.md) later for
automatic weighing (and it's the only way to get NFC on a Panda Touch
build).

---

## Get building

1. Get [Bambuddy](https://github.com/maziggy/bambuddy) running and grab an
   API key from its device settings.
2. Pick a console above and open its page for the parts list, wiring, and
   case options.
3. Follow the [setup & flashing guide](docs/setup.md) — install ESPHome,
   fill in your WiFi/Bambuddy details, and flash it.
4. Optional: build a [Scale](docs/scale.md) too, for automatic spool
   weighing.

---

## Using it day to day

Scan a tag, load the spool, watch it get assigned in Bambuddy — most of it
just works without you thinking about it. See
**[Using the device](docs/usage.md)** for the full walkthrough: auto-assign,
unlinked tags, weighing, writing tags, sleep behavior, and what the status
LEDs mean.

---

## More documentation

- **[Setup & flashing](docs/setup.md)** — install ESPHome, secrets, first
  flash, OTA updates
- **[Configuration reference](docs/configuration.md)** — every tunable
  setting, what it does, and its default
- **[API reference](docs/api-reference.md)** — the Bambuddy endpoints this
  firmware calls, for anyone working on the backend side
- **[Troubleshooting](docs/troubleshooting.md)** — fixes for the most common
  hiccups
- **[Development / repo layout](docs/development.md)** — for anyone poking
  at the code

---

## Credits & related projects

This project stands on the shoulders of two other open-source projects:

- **[Bambuddy](https://github.com/maziggy/bambuddy)** by
  [maziggy](https://github.com/maziggy) — the backend server this firmware
  talks to. It manages your spool inventory, Bambu Lab printer/AMS polling,
  and the original Raspberry-Pi SpoolBuddy client that this firmware
  reimplements. **You need a running Bambuddy instance before an
  ESPoolBuddy device is useful** — see its repo for setup.
- **[SpoolEase](https://github.com/yanshay/SpoolEase)** by
  [yanshay](https://github.com/yanshay) ([spoolease.io](https://www.spoolease.io/))
  — the hardware design the WT32-SC01 Plus console is built around. The
  console/scale wiring and two-device concept are based on SpoolEase's
  excellent [hardware build guide](https://docs.spoolease.io/docs/build-setup/console-build)
  and case designs on [MakerWorld](https://makerworld.com/en/models/1138678-spoolease-console-nfc-rfid-filament-management).
  ESPoolBuddy reuses that hardware but is a from-scratch ESPHome firmware
  speaking the Bambuddy API — it's not SpoolEase's firmware and isn't
  affiliated with or supported by the SpoolEase project.

If you're already running Bambuddy, maybe already have SpoolEase hardware on
hand, and want a dedicated NFC console/scale for it, you're in the right
place.

---

## License & Disclaimer

**AGPL-3.0** (GNU Affero General Public License v3.0) — see [LICENSE](LICENSE).

This project is provided for informational and educational purposes only.
By using it, you accept the following:

- **Work at your own risk** — you're solely responsible for your safety and
  the proper handling of all electrical components.
- **No liability** — I'm not liable for any damages, injuries, or losses
  resulting from the use or misuse of the information and files provided.
- **No warranty** — this has been tested on my own workbench, but there's
  no guarantee it'll work flawlessly in your environment, given variations
  in component quality and assembly skill.

---

## Support this project

If ESPoolBuddy saved you a trip to the hardware store or an evening of
sorting spools, consider [starring the repo](https://github.com/CSchlipp/espoolbuddy) —
it helps others find it — or supporting ongoing development via
[Buy Me a Coffee](https://www.buymeacoffee.com/cschlipp) or
[Ko-fi](https://ko-fi.com/cschlipp). Any of these is appreciated, none is
expected.
