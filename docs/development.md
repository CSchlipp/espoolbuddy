← [Back to README](../README.md)

# Development / repository layout

For anyone poking at the code rather than just building the hardware:

```
espoolbuddy/
├── espoolbuddy_console.yaml            # Console entry point (WT32-SC01 Plus: display + NFC + speaker)
├── espoolbuddy_console_pandatouch.yaml # Alternate console entry point (Panda Touch: display only)
├── espoolbuddy_scale.yaml              # Scale entry point (HX711 + NFC, headless)
├── secrets.yaml.example               # Template — copy to secrets.yaml (git-ignored)
├── espoolbuddy/                       # Console-only packages, shared by both console entry points
│   ├── version.yaml                   #   Single SemVer version shared by every device variant
│   ├── app.yaml                       #   UI logic, sleep state machine, NFC/scale wiring, sensors
│   ├── lvgl.yaml                      #   LVGL screen/widget layout (resolution-independent)
│   ├── assets.yaml                    #   Fonts (MDI webfont) and image declarations
│   ├── images/                        #   PNG icons (spool fill/shine/empty, AMS icons)
│                                      #   downloaded from GitHub at compile time, see below
│   └── lvgl/                          #   Per-tab LVGL definitions (AMS tab, NFC tab)
├── components/                        # Shared ESPHome external_components
│   ├── bambuddy_api/                  #   HTTP client/server + Bambuddy API protocol (C++)
│   └── bambuddy_nfc/                  #   PN532 driver + Bambu MIFARE key derivation (C++)
├── docs/                              # This documentation, plus wiring/architecture diagrams
└── .github/workflows/                 # CI: compiles all three configs against the latest ESPHome release
```

`espoolbuddy_console.yaml` and `espoolbuddy_console_pandatouch.yaml` pull in
the same `espoolbuddy/*.yaml` packages — the UI and app logic are written
once and shared; only hardware pinout/wiring differs between the two files.

`components/` is pulled by all three entry-point YAMLs via
`external_components: source: git ...` pointed at this repo's own `main`
branch, so each YAML also works as a standalone copy-paste example. The PNGs
in `espoolbuddy/images/` are fetched the same way: `assets.yaml` builds their
URLs from an `espoolbuddy_images` substitution pinned to the same
`${espoolbuddy_ref}`, and ESPHome downloads and caches them at compile time
(under `.esphome/image/`) rather than reading them off disk. When developing
locally, CI rewrites the components, packages and image sources to the local
checkout before compiling — see `.github/scripts/use_local_components.py` if
you're doing the same.

No other build tooling, no lint step, no unit tests — the closest thing to
a test suite is a full firmware compile of all three configs, both locally
(`esphome compile <file>`) and in CI on every push/PR.

**LVGL lists are static YAML by default.** Every list/grid in the UI (the
NFC spool picker, the printer picker, the AMS grid) is hand-unrolled in YAML
with a fixed cap and per-slot hide/show, since ESPHome's declarative LVGL
config has no "repeat N times" construct. The one exception is the Storage
Locations screen (Settings), whose rows are built and torn down at runtime
with raw LVGL C++ calls inside a `script:` lambda (`render_storage_locations`
in `app.yaml`) — there's no reasonable static cap for an externally-managed,
unbounded list of locations. That pattern is deliberately scoped to that one
screen rather than adopted everywhere; see the comment above
`render_storage_locations` before reusing it elsewhere.
