← [Back to README](../README.md)

# Setup & flashing

These steps are the same for every device this project builds — the
console (either hardware option) and the scale. Wherever you see
**`<console file>`** below, use the entry-point YAML for whichever device
you're flashing:

| Device | File |
|---|---|
| Console — WT32-SC01 Plus | `espoolbuddy_console.yaml` |
| Console — Panda Touch | `espoolbuddy_console_pandatouch.yaml` |
| Scale | `espoolbuddy_scale.yaml` |

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

## 3. Clone this repo and set up secrets

```bash
git clone https://github.com/CSchlipp/espoolbuddy.git
cd espoolbuddy
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml`:

```yaml
wifi_ssid: "YourWiFiSSID"
wifi_password: "YourWiFiPassword"

bambuddy_backend_url: "http://192.168.1.100:5000"   # your Bambuddy server
bambuddy_api_key: "your-api-key-from-bambuddy-settings"

ota_password: "your-ota-password"
api_encryption_key: "REPLACE_WITH_YOUR_OWN_KEY"      # generate: openssl rand -base64 32
```

One `secrets.yaml` is shared by every device — the scale just ignores
`bambuddy_backend_url` / `bambuddy_api_key` since it never talks to Bambuddy
directly.

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
