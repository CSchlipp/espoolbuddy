#!/usr/bin/env python3
"""Regenerate the UI screenshots in docs/images/ui/ from the desktop simulator.

Compiles host/docs_screenshots.yaml at each full-console resolution and
host/docs_screenshots_lite.yaml at the Lite resolution, runs each (it walks
the UI through every documented screen, snapshots each one and exits on its
own), and writes the results as PNGs into docs/images/ui/full/ and
docs/images/ui/lite/. Screens both UIs have get the same file name in both
folders (see SHARED below).

Run it with the Python that has ESPHome installed, from anywhere:

    python host/update_screenshots.py            # every variant
    python host/update_screenshots.py 172x320    # just the listed resolution(s)

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
OUT_FULL = REPO / "docs" / "images" / "ui" / "full"
OUT_LITE = REPO / "docs" / "images" / "ui" / "lite"

# Screenshot run configs and their esphome.name (which sets the build dir and
# the snapshot folder).
FULL = (REPO / "host" / "docs_screenshots.yaml", "espoolbuddy-docs")
LITE = (REPO / "host" / "docs_screenshots_lite.yaml", "espoolbuddy-docs-lite")

# Screens both UIs have. Both walkthroughs (docs_screenshots.yaml and
# docs_screenshots_lite.yaml) take them under these names, from the same demo
# data, so docs/images/ui/full/<name>.png and docs/images/ui/lite/<name>.png
# always show the same situation on the two consoles.
SHARED = [
    "scale", "scale-settling", "scale-no-scale",
    "nfc-waiting", "nfc-spool", "quick-weight", "weight-popup", "location",
    "unlink-confirm", "assigned", "ams-removed", "archive", "timeout",
    "nfc-unlinked", "nfc-unlinked-bambu", "nfc-unlinked-opentag3d", "created",
    "plate-clear", "calibrate",
]
# Screens only one UI has.
FULL_ONLY = ["ams", "ams-slot", "quick-settings", "nfc-picker", "spool-keypad",
             "tag-menu", "tag-write-waiting", "tag-write-done", "tag-chip",
             "settings-features", "settings-storage"]
LITE_ONLY = ["spool-actions", "unlinked-actions", "settings", "settings-more"]

# (width, height, {snapshot name: output file name}, run config, output dir).
# The WT32-SC01 Plus renders the full set; the Panda Touch only gets two shots
# on its own page.
VARIANTS = [
    (480, 320, {n: f"{n}.png" for n in SHARED + FULL_ONLY}, FULL, OUT_FULL),
    (800, 480, {
        "ams": "pandatouch-ams.png",
        "nfc-spool": "pandatouch-nfc-spool.png",
    }, FULL, OUT_FULL),
    (172, 320, {n: f"{n}.png" for n in SHARED + LITE_ONLY}, LITE, OUT_LITE),
]

RUN_TIMEOUT_S = 240


def run_variant(width: int, height: int, shots: dict[str, str],
                run: tuple[pathlib.Path, str], out: pathlib.Path) -> None:
    config, name = run
    build = REPO / "host" / ".esphome" / "build" / name / ".pioenvs" / name / "program"
    snapshots = REPO / "host" / ".esphome" / "snapshots" / name
    print(f"==> {width}x{height}: compiling {config.name}", flush=True)
    subprocess.run(
        [sys.executable, "-m", "esphome",
         "-s", "screen_width", str(width), "-s", "screen_height", str(height),
         "compile", str(config)],
        check=True,
    )

    shutil.rmtree(snapshots, ignore_errors=True)
    with tempfile.TemporaryDirectory() as prefdir:
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", ESPHOME_PREFDIR=prefdir)
        print(f"==> {width}x{height}: capturing", flush=True)
        result = subprocess.run(
            [str(build)], env=env, timeout=RUN_TIMEOUT_S,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
    if result.returncode != 0:
        print(result.stdout)
        sys.exit(f"simulator exited with code {result.returncode}")

    out.mkdir(parents=True, exist_ok=True)
    for shot, filename in shots.items():
        src = snapshots / f"{shot}.bmp"
        if not src.exists():
            print(result.stdout)
            sys.exit(f"missing snapshot {src.name} — did its step in "
                     f"{config.name} fail?")
        Image.open(src).convert("RGB").save(out / filename, optimize=True)
        print(f"    {(out / filename).relative_to(REPO)}")


def main() -> None:
    only = set(sys.argv[1:])
    for width, height, shots, run, out in VARIANTS:
        if only and f"{width}x{height}" not in only:
            continue
        run_variant(width, height, shots, run, out)
    print("Done. Review the changes with `git diff --stat docs/images/ui`.")


if __name__ == "__main__":
    main()
