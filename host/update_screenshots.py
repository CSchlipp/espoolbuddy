#!/usr/bin/env python3
"""Regenerate the UI screenshots in docs/images/ui/ from the desktop simulator.

Compiles host/docs_screenshots.yaml at each console resolution, runs it
(it walks the UI through every documented screen, snapshots each one and
exits on its own), and writes the results as PNGs into docs/images/ui/.

Run it with the Python that has ESPHome installed, from anywhere:

    python host/update_screenshots.py

Needs the same SDL2 development files as the simulator itself (sdl2-config
on PATH). No display is needed: SDL's dummy video driver renders off-screen.
Pillow, used for the BMP -> PNG conversion, is an ESPHome dependency.

Each run starts from empty preferences (a fresh ESPHOME_PREFDIR), so the
output doesn't depend on what an earlier simulator session saved.
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
CONFIG = REPO / "host" / "docs_screenshots.yaml"
NAME = "espoolbuddy-docs"  # esphome.name in docs_screenshots.yaml
BUILD = REPO / "host" / ".esphome" / "build" / NAME / ".pioenvs" / NAME / "program"
SNAPSHOTS = REPO / "host" / ".esphome" / "snapshots" / NAME
OUT = REPO / "docs" / "images" / "ui"

# (width, height, {snapshot name: output file name}). The WT32-SC01 Plus set
# is what the docs use throughout; the Panda Touch only gets two shots on
# its own page.
VARIANTS = [
    (480, 320, {
        "ams": "ams.png",
        "ams-slot": "ams-slot.png",
        "quick-settings": "quick-settings.png",
        "nfc-spool": "nfc-spool.png",
        "nfc-unlinked": "nfc-unlinked.png",
        "nfc-picker": "nfc-picker.png",
        "scale": "scale.png",
        "settings-features": "settings-features.png",
        "settings-storage": "settings-storage.png",
    }),
    (800, 480, {
        "ams": "pandatouch-ams.png",
        "nfc-spool": "pandatouch-nfc-spool.png",
    }),
]

RUN_TIMEOUT_S = 120


def run_variant(width: int, height: int, shots: dict[str, str]) -> None:
    print(f"==> {width}x{height}: compiling", flush=True)
    subprocess.run(
        [sys.executable, "-m", "esphome",
         "-s", "screen_width", str(width), "-s", "screen_height", str(height),
         "compile", str(CONFIG)],
        check=True,
    )

    shutil.rmtree(SNAPSHOTS, ignore_errors=True)
    with tempfile.TemporaryDirectory() as prefdir:
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", ESPHOME_PREFDIR=prefdir)
        print(f"==> {width}x{height}: capturing", flush=True)
        result = subprocess.run(
            [str(BUILD)], env=env, timeout=RUN_TIMEOUT_S,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
    if result.returncode != 0:
        print(result.stdout)
        sys.exit(f"simulator exited with code {result.returncode}")

    OUT.mkdir(parents=True, exist_ok=True)
    for shot, filename in shots.items():
        src = SNAPSHOTS / f"{shot}.bmp"
        if not src.exists():
            print(result.stdout)
            sys.exit(f"missing snapshot {src.name} — did its step in "
                     f"{CONFIG.name} fail?")
        Image.open(src).convert("RGB").save(OUT / filename, optimize=True)
        print(f"    {OUT.relative_to(REPO) / filename}")


def main() -> None:
    for width, height, shots in VARIANTS:
        run_variant(width, height, shots)
    print("Done. Review the changes with `git diff --stat docs/images/ui`.")


if __name__ == "__main__":
    main()
