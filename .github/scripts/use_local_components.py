#!/usr/bin/env python3
"""Rewrite a SpoolBuddy config's remote GitHub sources to local ones.

The real configs (espoolbuddy_console*.yaml / espoolbuddy_scale.yaml) ship
inline GitHub `main` sources for both external_components (the C++
bambuddy_api/bambuddy_nfc components) and packages (the espoolbuddy/*.yaml
UI/app files), so they double as standalone copy & paste examples. The
console UI's images are fetched from the same GitHub ref by the
espoolbuddy_images substitution in espoolbuddy/assets.yaml. CI calls this
script to point all three at the local checkout instead, so the pipeline
builds the code in the checked-out commit rather than main.

It matches each block as a literal string and aborts if it is missing, which
keeps CI honest: change any of them and this script must be updated too (the
build fails with a clear message until it is).

Console configs pick their UI with one bundle file from espoolbuddy/bundles/
(files: [bundles/console_full.yaml] or files: [bundles/console_lite.yaml]);
the legacy four-file list (version/app/assets/lvgl) that existing users'
copied configs still carry is accepted too. The scale config includes its
bundle the same way (files: [bundles/scale.yaml]); its legacy form,
files: [version.yaml], is accepted as well. CI proves the legacy console
layout still builds with .github/legacy/espoolbuddy_console.yaml.

Usage: use_local_components.py <config.yaml> [...]
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

def _packages_git(files: str) -> str:
    return f"""packages:
  espoolbuddy:
    url: https://github.com/CSchlipp/espoolbuddy.git
    ref: ${{espoolbuddy_ref}}
    path: espoolbuddy
    files: [{files}]
    refresh: always"""


# Legacy console form: the four UI files listed one by one.
PACKAGES_GIT_CONSOLE = _packages_git("version.yaml, app.yaml, assets.yaml, lvgl.yaml")

PACKAGES_LOCAL_CONSOLE = """packages:
  version: !include espoolbuddy/version.yaml
  app:     !include espoolbuddy/app.yaml
  assets:  !include espoolbuddy/assets.yaml
  ui:      !include espoolbuddy/lvgl.yaml"""

# Bundle forms: one file that includes everything its UI needs.
PACKAGES_GIT_FULL = _packages_git("bundles/console_full.yaml")
PACKAGES_LOCAL_FULL = """packages:
  ui: !include espoolbuddy/bundles/console_full.yaml"""

PACKAGES_GIT_LITE = _packages_git("bundles/console_lite.yaml")
PACKAGES_LOCAL_LITE = """packages:
  ui: !include espoolbuddy/bundles/console_lite.yaml"""

# (git block, local block) for every console form, checked in this order.
CONSOLE_PACKAGES = [
    (PACKAGES_GIT_FULL, PACKAGES_LOCAL_FULL),
    (PACKAGES_GIT_LITE, PACKAGES_LOCAL_LITE),
    (PACKAGES_GIT_CONSOLE, PACKAGES_LOCAL_CONSOLE),
]

# The scale: its bundle, or the legacy version-only list (logic inline).
SCALE_PACKAGES = [
    (_packages_git("bundles/scale.yaml"), """packages:
  scale: !include espoolbuddy/bundles/scale.yaml"""),
    (_packages_git("version.yaml"), """packages:
  version: !include espoolbuddy/version.yaml"""),
]

# espoolbuddy/assets.yaml's image base. Relative to the config dir (= repo
# root in CI), so the local form resolves to the checked-out PNGs.
ASSETS_YAML = pathlib.Path("espoolbuddy/assets.yaml")

IMAGES_GIT = (
    "  espoolbuddy_images: https://raw.githubusercontent.com/CSchlipp/"
    "espoolbuddy/${espoolbuddy_ref}/espoolbuddy/images"
)

IMAGES_LOCAL = "  espoolbuddy_images: espoolbuddy/images"


def rewrite_assets(path: pathlib.Path) -> None:
    """Point assets.yaml's image base at the local checkout.

    Only called for the console configs — the scale config has no display and
    never includes assets.yaml.
    """
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")

    if IMAGES_LOCAL in text:
        print(f"Image base already local in {path}")
        return

    if IMAGES_GIT not in text:
        sys.exit(
            f"::error file={path}::expected espoolbuddy_images substitution not "
            f"found. If assets.yaml's image base changed, update IMAGES_GIT in "
            f".github/scripts/use_local_components.py to match."
        )

    path.write_text(text.replace(IMAGES_GIT, IMAGES_LOCAL), encoding="utf-8")
    print(f"Rewrote image base -> local source in {path}")


def rewrite(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")

    if GIT_BLOCK not in text:
        sys.exit(
            f"::error file={path}::expected external_components git block not "
            f"found. If the configs' external_components block changed, update "
            f"GIT_BLOCK in .github/scripts/use_local_components.py to match."
        )
    text = text.replace(GIT_BLOCK, LOCAL_BLOCK)

    console = next(((g, l) for g, l in CONSOLE_PACKAGES if g in text), None)
    is_console = console is not None
    if is_console:
        text = text.replace(*console)
    elif (scale := next(((g, l) for g, l in SCALE_PACKAGES if g in text), None)):
        text = text.replace(*scale)
    else:
        sys.exit(
            f"::error file={path}::expected packages git block not found. "
            f"If the configs' packages block changed, update "
            f"CONSOLE_PACKAGES/SCALE_PACKAGES in "
            f".github/scripts/use_local_components.py to match."
        )

    path.write_text(text, encoding="utf-8")
    print(f"Rewrote external_components + packages -> local source in {path}")

    if is_console:
        rewrite_assets(path.parent / ASSETS_YAML)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: use_local_components.py <config.yaml> [...]")
    for arg in sys.argv[1:]:
        rewrite(pathlib.Path(arg))
