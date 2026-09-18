← [Back to README](../README.md)

# Setup & flashing

These steps are the same for every device this project builds — the
console (either hardware option) and the scale. Wherever you see
**`<console file>`** below, use the entry-point YAML for whichever device
you're flashing:

| Device | File |
|---|---|
| Console — WT32-SC01 Plus | [`espoolbuddy_console.yaml`](../espoolbuddy_console.yaml) |
| Console — Panda Touch | [`espoolbuddy_console_pandatouch.yaml`](../espoolbuddy_console_pandatouch.yaml) |
| Scale | [`espoolbuddy_scale.yaml`](../espoolbuddy_scale.yaml) |

## 1. Get Bambuddy running first

ESPoolBuddy is a client for [Bambuddy](https://github.com/maziggy/bambuddy).
Install and configure it first (it needs your Bambu Lab printer's LAN IP +
access code, or cloud credentials), then create an API key for this device
under Bambuddy's device settings — you'll need it below.

## 2. Install ESPHome

Follow the official
**[ESPHome Getting Started guide](https://esphome.io/guides/getting_started_command_line/)**.
It covers the CLI (`pip install esphome`), the desktop ESPHome Dashboard, and
the Home Assistant Add-on, and stays accurate as ESPHome's install process
evolves. Any of those work here; the commands below assume the CLI.

## 3. Get your device's config file and set up secrets

There's nothing to clone. Each entry-point YAML is self-contained — it pulls
the C++ components, the shared UI packages and (on a console) the images from
this repo over the network at build time. Your ESPHome config directory needs
just two files: your device's YAML, and a `secrets.yaml` beside it.

With the CLI, download the one file for your device:

```bash
curl -O https://raw.githubusercontent.com/CSchlipp/espoolbuddy/main/<console file>
```

In the ESPHome Dashboard or the Home Assistant add-on, create a new device
instead and replace the YAML it generates with the contents of the file
linked in the table above.

Then create `secrets.yaml` next to it:

```yaml
wifi_ssid: "YourWiFiSSID"
wifi_password: "YourWiFiPassword"
ap_fallback_password: "your-fallback-ap-password"   # fallback hotspot, 8+ characters

bambuddy_backend_url: "http://192.168.1.100:5000"   # your Bambuddy server
bambuddy_api_key: "your-api-key-from-bambuddy-settings"

api_encryption_key: "REPLACE_WITH_YOUR_OWN_KEY"      # generate: openssl rand -base64 32
```

One `secrets.yaml` is shared by every device — the scale just ignores
`bambuddy_backend_url` / `bambuddy_api_key` since it never talks to Bambuddy
directly.

### Tracking `main` vs. pinning a release

Out of the box a device builds against this repo's `main` branch, so every
flash picks up the latest firmware. To hold one on a known-good release
instead, change the single substitution at the top of its YAML:

```yaml
substitutions:
  espoolbuddy_ref: v0.27.1   # instead of: main
```

That one value covers the components, the shared UI packages and the images
alike — see [releases](https://github.com/CSchlipp/espoolbuddy/releases) for
what's available.

Planning to modify the firmware itself rather than just flash it? Clone the
repo and see the [development guide](development.md) for the layout.

## 4. First flash (over USB)

Wire the hardware first (see the device's own page), then connect it via USB
and flash it directly — OTA only works once the firmware is already on the
device:

```bash
esphome run <console file>
```

This compiles, flashes, and then streams logs, so you can confirm WiFi
connects and (for a console) that it registers with Bambuddy.

## 5. Subsequent updates (over WiFi / OTA)

Once a device has flashed and joined WiFi, you can re-flash it wirelessly —
the same command auto-detects it on the network:

```bash
esphome run <console file>
```

Uploads are authenticated and encrypted with your `api_encryption_key` — the
same key the Home Assistant API uses — so there is no separate OTA password to
keep in `secrets.yaml`.

> **Updating a device that was flashed before this repo dropped the OTA
> password?** That firmware still asks for a password the uploader no longer
> sends, so its next upload has to go over USB. Every OTA after that one works
> as above.
