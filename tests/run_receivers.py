"""Run real browser checks against isolated simulated DAW instances."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
host = root / "build/MConnectHarness_artefacts/Release/MConnect test host.app/Contents/MacOS/MConnect test host"
results = []
for rate, engine, block in [(48000,"chromium",256),(48000,"webkit",256),(44100,"chromium",256),(96000,"chromium",256),(192000,"chromium",256),(48000,"chromium",1024),(48000,"chromium",2048)]:
    with tempfile.TemporaryDirectory(prefix="mconnect-receiver-",dir="/private/tmp" if sys.platform == "darwin" else None) as temporary:
        info = Path(temporary) / "info.json"
        process = subprocess.Popen([str(host),"--no-editor","--rate",str(rate),"--block",str(block),"--seconds","120","--info",str(info)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        try:
            for _ in range(200):
                try:
                    json.loads(info.read_text()); break
                except (FileNotFoundError,json.JSONDecodeError): time.sleep(.1)
            if not info.exists(): raise RuntimeError(f"Test host did not start (exit={process.poll()})")
            outcome = subprocess.run(["node","tests/receiver.cjs",str(info),engine],cwd=root,capture_output=True,text=True,timeout=60)
            print(outcome.stdout or outcome.stderr,flush=True)
            if outcome.returncode: raise SystemExit(outcome.returncode)
            results.append(json.loads(outcome.stdout.strip()))
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
(root / "test-results").mkdir(exist_ok=True)
(root / "test-results/browser-matrix.json").write_text(json.dumps(results,indent=2))
