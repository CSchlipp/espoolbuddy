#!/usr/bin/env python3
"""Rewrite a SpoolBuddy config's remote GitHub sources to local ones.

The real configs (espoolbuddy_console.yaml / espoolbuddy_scale.yaml) ship
inline GitHub `main` sources for both external_components (the C++
bambuddy_api/bambuddy_nfc components) and packages (the espoolbuddy/*.yaml
UI/app files), so they double as standalone copy & paste examples. CI calls
this script to point both blocks at the local checkout instead, so the
pipeline builds the code in the checked-out commit rather than main.

It matches each block as a literal string and aborts if it is missing, which
keeps CI honest: change either block in the configs and this script must be
updated too (the build fails with a clear message until it is).

Usage: use_local_components.py <config.yaml> [<config.yaml> ...]
"""
import pathlib
import sys

GIT_BLOCK = """external_components:
  - source:
      type: git
      url: https://github.com/CSchlipp/espoolbuddy.git
      ref: ${espoolbuddy_ref}
      path: components
    components: [bambuddy_api, bambuddy_nfc]
    refresh: always"""

LOCAL_BLOCK = """external_components:
  - source:
      type: local
      path: ./components
    components: [bambuddy_api, bambuddy_nfc]"""

PACKAGES_GIT_CONSOLE = """packages:
  espoolbuddy:
    url: https://github.com/CSchlipp/espoolbuddy.git
    ref: ${espoolbuddy_ref}
    path: espoolbuddy
    files: [version.yaml, app.yaml, assets.yaml, lvgl.yaml]
    refresh: always"""

PACKAGES_LOCAL_CONSOLE = """packages:
  version: !include espoolbuddy/version.yaml
  app:     !include espoolbuddy/app.yaml
  assets:  !include espoolbuddy/assets.yaml
  ui:      !include espoolbuddy/lvgl.yaml"""

PACKAGES_GIT_SCALE = """packages:
  espoolbuddy:
    url: https://github.com/CSchlipp/espoolbuddy.git
    ref: ${espoolbuddy_ref}
    path: espoolbuddy
    files: [version.yaml]
    refresh: always"""

PACKAGES_LOCAL_SCALE = """packages:
  version: !include espoolbuddy/version.yaml"""


def rewrite(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")

    if GIT_BLOCK not in text:
        sys.exit(
            f"::error file={path}::expected external_components git block not "
            f"found. If the configs' external_components block changed, update "
            f"GIT_BLOCK in .github/scripts/use_local_components.py to match."
        )
    text = text.replace(GIT_BLOCK, LOCAL_BLOCK)

    if PACKAGES_GIT_CONSOLE in text:
        text = text.replace(PACKAGES_GIT_CONSOLE, PACKAGES_LOCAL_CONSOLE)
    elif PACKAGES_GIT_SCALE in text:
        text = text.replace(PACKAGES_GIT_SCALE, PACKAGES_LOCAL_SCALE)
    else:
        sys.exit(
            f"::error file={path}::expected packages git block not found. "
            f"If the configs' packages block changed, update "
            f"PACKAGES_GIT_CONSOLE/PACKAGES_GIT_SCALE in "
            f".github/scripts/use_local_components.py to match."
        )

    path.write_text(text, encoding="utf-8")
    print(f"Rewrote external_components + packages -> local source in {path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: use_local_components.py <config.yaml> [...]")
    for arg in sys.argv[1:]:
        rewrite(pathlib.Path(arg))
