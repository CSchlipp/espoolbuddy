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
│   ├── images/                        #   PNG icons (spool fill/hub/shine/empty, AMS icons)
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
branch, so each YAML also works as a standalone copy-paste example. When
developing components locally, CI rewrites that block to a local
`./components` checkout before compiling — see
`.github/scripts/use_local_components.py` if you're doing the same.

No other build tooling, no lint step, no unit tests — the closest thing to
a test suite is a full firmware compile of all three configs, both locally
(`esphome compile <file>`) and in CI on every push/PR.
