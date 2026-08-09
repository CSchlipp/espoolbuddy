#!/usr/bin/env python3
"""Rewrite a SpoolBuddy config's external_components block to a local source.

The real configs (espoolbuddy_console.yaml / espoolbuddy_scale.yaml) ship an
inline GitHub `main` source so they double as standalone copy & paste examples.
CI calls this script to point that block at ./components instead, so the
pipeline builds the code in the checked-out commit rather than main.

It matches the git block as a literal string and aborts if it is missing, which
keeps CI honest: change the configs' external_components block and this script
must be updated too (the build fails with a clear message until it is).

Usage: use_local_components.py <config.yaml> [<config.yaml> ...]
"""
import pathlib
import sys

GIT_BLOCK = """external_components:
  - source:
      type: git
      url: https://github.com/CSchlipp/espoolbuddy.git
      ref: main
      path: components
    components: [bambuddy_api, bambuddy_nfc]"""

LOCAL_BLOCK = """external_components:
  - source:
      type: local
      path: ./components
    components: [bambuddy_api, bambuddy_nfc]"""


def rewrite(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if GIT_BLOCK not in text:
        sys.exit(
            f"::error file={path}::expected external_components git block not "
            f"found. If the configs' external_components block changed, update "
            f"GIT_BLOCK in .github/scripts/use_local_components.py to match."
        )
    path.write_text(text.replace(GIT_BLOCK, LOCAL_BLOCK), encoding="utf-8")
    print(f"Rewrote external_components -> local source in {path}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: use_local_components.py <config.yaml> [...]")
    for arg in sys.argv[1:]:
        rewrite(pathlib.Path(arg))
