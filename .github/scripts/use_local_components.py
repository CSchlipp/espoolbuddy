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

With --label-printer=<printer> it also adds an optional label printer
package to each console config and points that package's components
at the local checkout, so CI builds the label printer on top of an existing
config instead of a separate one. <printer> is one of LABEL_PRINTERS below
(m220 or brother_ql); a package that needs a setting with no default (the
Brother QL's address) gets a placeholder in the config's substitutions.

Usage: use_local_components.py [--label-printer=<printer>] <config.yaml> [...]
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


# The optional label printer packages: the package file, the components its
# external_components block lists, and the substitutions it needs set.
# 192.0.2.1 is a documentation address (RFC 5737): it only has to validate.
LABEL_PRINTERS = {
    "m220": (
        "label_printer_m220.yaml",
        "label_printer, label_printer_phomemo, label_printer_ble",
        {},
    ),
    "brother_ql": (
        "label_printer_brother_ql.yaml",
        "label_printer, label_printer_brother_ql, label_printer_tcp",
        {"label_printer_host": "192.0.2.1"},
    ),
}


def _label_printer_git(components: str) -> str:
    return f"""external_components:
  - source:
      type: git
      url: https://github.com/CSchlipp/espoolbuddy.git
      ref: ${{espoolbuddy_ref}}
      path: components
    components: [{components}]
    refresh: always"""


def _label_printer_local(components: str) -> str:
    return f"""external_components:
  - source:
      type: local
      path: ./components
    components: [{components}]"""


def label_printer_include(printer: str) -> str:
    return f"\n  label_printer: !include espoolbuddy/{LABEL_PRINTERS[printer][0]}"


def add_label_printer_substitutions(text: str, printer: str, path: pathlib.Path) -> str:
    """Set the substitutions the package can't default, at the top of the config's own."""
    subs = LABEL_PRINTERS[printer][2]
    if not subs:
        return text
    if "\nsubstitutions:\n" not in text:
        sys.exit(f"::error file={path}::no substitutions: block to add {', '.join(subs)} to.")
    lines = "".join(f"  {k}: {v}  # set by use_local_components.py\n" for k, v in subs.items())
    return text.replace("\nsubstitutions:\n", "\nsubstitutions:\n" + lines, 1)


def rewrite_label_printer(path: pathlib.Path, printer: str) -> None:
    """Point the label printer package's components at the local checkout."""
    components = LABEL_PRINTERS[printer][1]
    git, local = _label_printer_git(components), _label_printer_local(components)
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")

    if local in text:
        print(f"Label printer components already local in {path}")
        return

    if git not in text:
        sys.exit(
            f"::error file={path}::expected external_components git block not "
            f"found. If the label printer package's block changed, update "
            f"LABEL_PRINTERS in .github/scripts/use_local_components.py to match."
        )

    path.write_text(text.replace(git, local), encoding="utf-8")
    print(f"Rewrote label printer components -> local source in {path}")


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


def rewrite(path: pathlib.Path, label_printer: str | None = None) -> None:
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
        git, local = console
        if label_printer:
            local += label_printer_include(label_printer)
            text = add_label_printer_substitutions(text, label_printer, path)
        text = text.replace(git, local)
    elif label_printer:
        sys.exit(f"::error file={path}::--label-printer needs a console config.")
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
    if label_printer:
        print(f"Added the {label_printer} label printer package to {path}")
        rewrite_label_printer(path.parent / "espoolbuddy" / LABEL_PRINTERS[label_printer][0], label_printer)


if __name__ == "__main__":
    usage = (
        "usage: use_local_components.py [--label-printer=<printer>] <config.yaml> [...]\n"
        f"<printer> is one of: {', '.join(LABEL_PRINTERS)}"
    )
    label_printer = None
    configs = []
    for a in sys.argv[1:]:
        if a == "--label-printer" or a.startswith("--label-printer="):
            label_printer = a.partition("=")[2]
            if label_printer not in LABEL_PRINTERS:
                sys.exit(usage)
        else:
            configs.append(a)
    if not configs:
        sys.exit(usage)
    for arg in configs:
        rewrite(pathlib.Path(arg), label_printer)
