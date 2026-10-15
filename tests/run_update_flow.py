#!/usr/bin/env python3
"""Opt-in live updater UI test. Uses GitHub and temporarily isolates the update cache.

The host emulates an older version using the current updater/editor code.
It opens no DAW or audio device. Click through its update UI, then close it.
The test cache is preserved under output/update-flow; the user's cache is restored.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--live-ui", action="store_true", required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    binary = args.build / "MConnectUpdateFlow_artefacts/Release/MConnect update flow test.app/Contents/MacOS/MConnect update flow test"
    if not binary.is_file():
        parser.error("Build MConnectUpdateFlow first")
    cache = Path.home() / "Library/Caches/MConnect/Updates"
    backup = cache.with_name("Updates.before-flow-" + uuid.uuid4().hex)
    if cache.is_symlink():
        raise RuntimeError("Refusing to move a symlink cache")
    output = ROOT / "output/update-flow" / uuid.uuid4().hex
    output.mkdir(parents=True)
    cache.parent.mkdir(parents=True, exist_ok=True)
    had_cache = cache.exists()
    if had_cache:
        cache.rename(backup)
    log = output / "ui.log"
    try:
        print(f"Live UI log: {log}", flush=True)
        with (output / "host.log").open("w") as stream:
            result = subprocess.run([str(binary), "--log", str(log)], stdout=stream, stderr=stream)
        if result.returncode:
            raise RuntimeError(f"Test host failed: {result.returncode}")
        if "verified=true" not in log.read_text():
            raise RuntimeError("No verified installer reached the ready state")
        print("PASS: live notification/download reached a verified installer; inspect UI log for handoff", flush=True)
    finally:
        # Restore the user's cache before copying evidence (which may fail if
        # the output volume is full). Keep the test cache on that failure.
        evidence = cache.with_name("Updates.after-flow-" + output.name)
        if cache.exists():
            cache.rename(evidence)
        if had_cache:
            backup.rename(cache)
        if evidence.exists():
            shutil.copytree(evidence, output / "cache")
            shutil.rmtree(evidence)
        print(f"User cache restored; evidence: {output}", flush=True)


if __name__ == "__main__":
    main()
