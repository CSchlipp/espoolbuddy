← [Back to README](../README.md)

# API reference

This is mostly useful if you're working on Bambuddy itself, or curious what
actually goes over the wire. The console consumes the Bambuddy API
**unchanged** — no server-side modifications are needed to use ESPoolBuddy.

## Endpoints used

| Method | Endpoint | Purpose |
|---|---|---|
| POST | `/api/v1/spoolbuddy/devices/register` | Register device on boot |
| POST | `/api/v1/spoolbuddy/devices/{id}/heartbeat` | Periodic heartbeat + command polling |
| POST | `/api/v1/spoolbuddy/nfc/tag-scanned` | NFC tag detected event |
| POST | `/api/v1/spoolbuddy/nfc/tag-removed` | NFC tag removed event |
| POST | `/api/v1/spoolbuddy/scale/reading` | Scale weight report |
| POST | `/api/v1/spoolbuddy/nfc/write-result` | Result of NTAG write operation |
| POST | `/api/v1/spoolbuddy/devices/{id}/calibration/set-tare` | Update tare offset |
| POST | `/api/v1/spoolbuddy/diagnostics/{id}/result` | Diagnostic run result |
| POST | `/api/v1/spoolbuddy/devices/{id}/system/command-result` | System command acknowledgement |

## Backend commands handled

Commands received in the `pending_command` field of the heartbeat response:

| Command | Action |
|---|---|
| `tare` | Zero the scale and report the new tare offset |
| `write_tag` | Write NDEF data to the next NTAG presented |
| `run_nfc_diag` / `run_scale_diag` / `run_read_tag_diag` | Return a mocked diagnostic result (no external scripts on ESP32) |
| `apply_system_config` | Update `backend_url` / `api_key` in RAM (reboot to persist) |
| `reboot` / `shutdown` / `restart_daemon` | `ESP.restart()` |
| `restart_browser` | No-op (no browser on ESP32) |

## SSH key deployment

The Bambuddy backend may send an SSH public key in registration/heartbeat
responses — on a Raspberry Pi SpoolBuddy this is written to
`~/.ssh/authorized_keys`. **ESPHome devices cannot accept SSH connections**,
so this component returns a mocked success response
(`"SSH not supported on ESPHome device"`) without performing any operation,
keeping the API contract intact.
