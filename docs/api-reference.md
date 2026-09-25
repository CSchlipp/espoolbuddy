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
| GET | `/api/v1/printers/{id}/status` | Printer/AMS state; `awaiting_plate_clear` drives the plate-clear popup |
| GET | `/api/v1/settings/ui-preferences` | Read `require_plate_clear` (the popup only shows when Bambuddy requires plate-clear confirmation) and `time_format` (header clock format) |
| POST | `/api/v1/printers/{id}/clear-plate` | Confirm the build plate is clear (popup **Confirm**) |
| GET | `/api/v1/spoolman/status` | Which inventory Bambuddy uses (Spoolman or its own database) |

## Inventory endpoints (internal vs. Spoolman)

The console asks Bambuddy which inventory it uses with `GET /spoolman/status`:
Spoolman when `enabled` is true and a `url` is set, otherwise Bambuddy's own
database. It checks once after registering and again every 5 minutes, so
switching in Bambuddy's settings needs no reflash. In Spoolman mode the console
still only talks to Bambuddy — Bambuddy proxies to Spoolman. All paths are under
`/api/v1`.

| Purpose | `internal` | `spoolman` |
|---|---|---|
| Get one spool | `GET /inventory/spools/{id}` | `GET /spoolman/inventory/spools/{id}` |
| List spools (NFC picker; spools that already have a tag are skipped) | `GET /inventory/spools` | `GET /spoolman/inventory/spools` |
| Link a tag to a spool | `PATCH /inventory/spools/{id}/link-tag` `{tag_uid, tray_uuid, tag_type, data_origin}` | `PATCH /spoolman/inventory/spools/{id}/tag` `{tag_uid, tray_uuid}` — `tray_uuid` is left out when empty (it must be 32 hex characters) |
| Unlink a tag | same endpoint, empty values | `PATCH /spoolman/inventory/spools/{id}` `{"tag_uid": null}` |
| Create a spool from a tag | `POST /inventory/spools` (tag in the body) | `POST /spoolman/inventory/spools`, then `PATCH …/{id}/tag` |
| Set / clear a spool's location | `PATCH /inventory/spools/{id}` `{"location_id": N \| null}` | `PATCH /spoolman/inventory/spools/{id}` (same body) |
| Archive a spool | `POST /inventory/spools/{id}/archive` | `POST /spoolman/inventory/spools/{id}/archive` |
| List slot assignments | `GET /inventory/assignments?printer_id=` (spool data is nested) | `GET /spoolman/inventory/slot-assignments/all?printer_id=` (ids only), plus one `GET /spoolman/inventory/spools/{id}` per assigned spool for weight, brand and colour |
| Assign a spool to a slot | `POST /inventory/assignments` `{spool_id, …}` | `POST /spoolman/inventory/slot-assignments` `{spoolman_spool_id, …}` |
| Unassign a slot | `DELETE /inventory/assignments/{printer}/{ams}/{tray}` | `DELETE /spoolman/inventory/slot-assignments/{spool_id}` |
| Storage locations: list, link/unlink a tag (`identifier`) | `GET` / `PATCH /inventory/locations[/{id}]` | same (Bambuddy keeps the catalog itself) |
| "Does a spool already own this tag?" (before linking a location) | `GET /inventory/spools/by-tag?tag_uid=` | scan of `GET /spoolman/inventory/spools` for a matching `tag_uid` or `tray_uuid` |
| Update spool weight | `POST /spoolbuddy/scale/update-spool-weight` (Bambuddy picks the backend) | same |

Spoolman-mode limits, all on Bambuddy's side: a spool created from a Bambu tag
gets no hotend temperatures, `tag_type` or `data_origin`, and the empty-spool
weight is not stored on the spool.

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
