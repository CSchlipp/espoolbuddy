← [Back to README](../README.md)

# Configuration reference

Edit the top of your device's entry-point YAML to change these —
[`espoolbuddy_console.yaml`](../espoolbuddy_console.yaml),
[`espoolbuddy_console_pandatouch.yaml`](../espoolbuddy_console_pandatouch.yaml),
or [`espoolbuddy_scale.yaml`](../espoolbuddy_scale.yaml). All three share the
same `bambuddy_api` component; several options only apply to one mode.

| Key | Applies to | Default | Description |
|---|---|---|---|
| `bambuddy_api.backend_url` | Console | *(secrets)* | Bambuddy server URL |
| `bambuddy_api.api_key` | Console | *(secrets)* | Bambuddy API key |
| `bambuddy_api.hostname` | Both | `SpoolBuddy-ESP` | Display name in the Bambuddy UI |
| `bambuddy_api.inventory_backend` | Console | `internal` | `internal` or `spoolman` — must match Bambuddy's Settings → Spoolman toggle |
| `bambuddy_api.heartbeat_interval` | Console | `10` s | Heartbeat / command-poll frequency |
| `bambuddy_api.printer_poll_interval` | Console | `30` s | AMS/printer state poll frequency |
| `bambuddy_api.sleep_timeout` | Console | `600` s | Idle time before deep sleep (`0` = disabled); also adjustable live from the Settings tab |
| `bambuddy_api.sleep_factor` | Console | `6` | Heartbeat/poll interval multiplier while asleep |
| `bambuddy_api.scale_mode` | Scale | `true` | Runs the local HTTP server + push client instead of talking to Bambuddy |
| `bambuddy_api.console_url` | Scale | `http://spoolbuddy-console.local` | Console URL(s) to push readings to — a single string or a list, see [Scale build](scale.md#point-it-at-the-console) |
| `bambuddy_api.scale_report_interval` | Scale | `100` ms | Weight push cadence to the console |
| `bambuddy_nfc.poll_interval` | Both | `300` ms | Fallback polling rate (only used if IRQ isn't wired) |
| `bambuddy_nfc.miss_threshold` | Both | `3` | Missed reads before a "tag removed" event fires |
| `bambuddy_api.clock_24h` | Console | `true` | Header clock format |

The Panda Touch console omits `nfc_id` / `speaker_id` entirely rather than
setting them — see its [known limitations](console-pandatouch.md#known-limitations)
for what that changes.

For the lower-level HTTP endpoints and commands these settings feed into,
see the [API reference](api-reference.md).
